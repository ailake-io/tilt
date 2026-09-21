"""Cliente Python da linguagem Tilt.

Chama funcoes e pipelines de um arquivo ``.tilt`` a partir do Python, sem
dependencias: por baixo, um processo ``tilt rpc`` fala JSON-lines por
stdin/stdout (ver docs/guia-17-interoperabilidade.md).

    import tilt

    vendas = tilt.carregar("exemplos/interop/vendas.tilt")
    vendas.classificar(120)                      # 'alto'
    vendas.total_por_regiao([{"regiao": "sul", "valor": 30}])
    vendas.so_altos(linhas, minimo=20)           # argumentos nomeados

    tilt.chamar("vendas.tilt", "classificar", 60)  # uma chamada, sem manter processo
"""

from __future__ import annotations

import json
import math
import os
import shutil
import subprocess
import threading
from typing import Any, Dict, List, Optional, Sequence

__all__ = ["Tilt", "TiltErro", "carregar", "chamar", "localizar_binario", "PROTOCOLO"]

PROTOCOLO = 1


class TiltErro(Exception):
    """Erro de execucao (ou de protocolo) devolvido pelo Tilt.

    ``saida`` guarda o que o programa imprimiu antes de falhar.
    """

    def __init__(self, mensagem: str, saida: str = "") -> None:
        super().__init__(mensagem)
        self.saida = saida


def localizar_binario(binario: Optional[str] = None) -> str:
    """Caminho do executavel ``tilt``: argumento, ``$TILT_BIN`` ou o ``PATH``."""
    candidato = binario or os.environ.get("TILT_BIN") or shutil.which("tilt")
    if not candidato:
        raise TiltErro(
            "executavel 'tilt' nao encontrado: instale-o no PATH ou defina TILT_BIN"
        )
    return candidato


def _para_json(valor: Any) -> Any:
    """Converte tipos comuns do ecossistema Python em algo serializavel."""
    if valor is None or isinstance(valor, (bool, int, str)):
        return valor
    if isinstance(valor, float):
        return None if math.isnan(valor) or math.isinf(valor) else valor
    if isinstance(valor, dict):
        return {str(k): _para_json(v) for k, v in valor.items()}
    if isinstance(valor, (list, tuple, set, frozenset)):
        return [_para_json(v) for v in valor]
    if hasattr(valor, "isoformat"):  # datetime / date / pandas.Timestamp
        return valor.isoformat()
    # pandas.DataFrame / polars / pyarrow: tabela vira lista de objetos.
    if hasattr(valor, "to_dict"):
        try:
            return _para_json(valor.to_dict("records"))
        except TypeError:
            return _para_json(valor.to_dict())
    if hasattr(valor, "to_pylist"):  # pyarrow.Table
        return _para_json(valor.to_pylist())
    if hasattr(valor, "tolist"):  # numpy
        return _para_json(valor.tolist())
    if hasattr(valor, "item"):  # escalares numpy
        return _para_json(valor.item())
    raise TypeError(f"tipo {type(valor).__name__} nao pode ser enviado ao Tilt")


class Tilt:
    """Um programa ``.tilt`` carregado em um processo ``tilt rpc``.

    Funcoes do programa viram metodos: ``prog.soma(1, 2)``. Use como gerenciador
    de contexto (``with``) ou chame :meth:`fechar`. Seguro entre threads (as
    chamadas sao serializadas por um lock).
    """

    def __init__(self, arquivo: str, binario: Optional[str] = None) -> None:
        self.arquivo = os.fspath(arquivo)
        self._lock = threading.Lock()
        self._proximo_id = 0
        self._proc: Optional[subprocess.Popen] = None
        self._proc = subprocess.Popen(
            [localizar_binario(binario), "rpc", self.arquivo],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
        )
        linha = self._proc.stdout.readline()
        if not linha:
            erro = self._proc.stderr.read().strip()
            self.fechar()
            raise TiltErro(erro or f"tilt rpc encerrou ao carregar {self.arquivo}")
        info = json.loads(linha)
        if info.get("protocolo") != PROTOCOLO:
            self.fechar()
            raise TiltErro(f"protocolo {info.get('protocolo')} nao suportado (esperado {PROTOCOLO})")
        self.versao: str = info.get("versao", "")
        self.funcoes: Dict[str, List[str]] = {f["nome"]: f["params"] for f in info["funcoes"]}
        self.pipelines: List[str] = list(info["pipelines"])

    # -- chamadas ----------------------------------------------------------
    def _requisitar(self, corpo: Dict[str, Any]) -> Dict[str, Any]:
        with self._lock:
            if self._proc is None or self._proc.poll() is not None:
                raise TiltErro("o processo tilt rpc nao esta mais ativo")
            self._proximo_id += 1
            corpo = dict(corpo, id=self._proximo_id)
            self._proc.stdin.write(json.dumps(corpo, ensure_ascii=False, allow_nan=False) + "\n")
            self._proc.stdin.flush()
            linha = self._proc.stdout.readline()
            if not linha:
                erro = self._proc.stderr.read().strip()
                raise TiltErro(erro or "tilt rpc encerrou sem responder")
            resposta = json.loads(linha)
        if not resposta.get("ok"):
            raise TiltErro(resposta.get("erro", "erro desconhecido"), resposta.get("saida", ""))
        return resposta

    def chamar(self, funcao: str, *args: Any, **nomeados: Any) -> Any:
        """Chama ``funcao`` e devolve o resultado (JSON convertido em tipos Python)."""
        return self.chamar_com_saida(funcao, *args, **nomeados)[0]

    def chamar_com_saida(self, funcao: str, *args: Any, **nomeados: Any):
        """Como :meth:`chamar`, mas devolve ``(resultado, saida_impressa)``."""
        corpo: Dict[str, Any] = {"chamar": funcao, "args": _para_json(list(args))}
        if nomeados:
            corpo["nomeados"] = _para_json(nomeados)
        resposta = self._requisitar(corpo)
        return resposta.get("resultado"), resposta.get("saida", "")

    def chamar_lote(self, funcao: str, lote: Sequence[Sequence[Any]]) -> List[Any]:
        """Chama ``funcao`` uma vez por item de ``lote`` (lista de listas de
        argumentos) em uma unica ida e volta ao processo. Devolve os resultados."""
        corpo = {"chamar": funcao, "lote": [_para_json(list(args)) for args in lote]}
        return self._requisitar(corpo).get("resultado", [])

    def executar_pipeline(self, nome: str) -> str:
        """Roda o pipeline ``nome``; devolve o que ele imprimiu."""
        return self._requisitar({"pipeline": nome}).get("saida", "")

    def ping(self) -> bool:
        return bool(self._requisitar({"ping": True}).get("ok"))

    def __getattr__(self, nome: str) -> Any:
        if nome.startswith("_") or "funcoes" not in self.__dict__:
            raise AttributeError(nome)
        if nome not in self.funcoes:
            raise AttributeError(f"o programa {self.arquivo!r} nao tem a funcao {nome!r}")
        return lambda *args, **nomeados: self.chamar(nome, *args, **nomeados)

    def __dir__(self) -> Sequence[str]:
        return sorted(set(super().__dir__()) | set(self.__dict__.get("funcoes", {})))

    # -- ciclo de vida -----------------------------------------------------
    def fechar(self) -> None:
        proc, self._proc = self._proc, None
        if proc is None:
            return
        try:
            proc.stdin.write('{"sair":true}\n')
            proc.stdin.flush()
        except (BrokenPipeError, OSError, ValueError):
            pass
        try:
            proc.stdin.close()
            proc.wait(timeout=5)
        except (subprocess.TimeoutExpired, OSError):
            proc.kill()
            proc.wait()
        for fluxo in (proc.stdout, proc.stderr):
            try:
                fluxo.close()
            except OSError:
                pass

    def __enter__(self) -> "Tilt":
        return self

    def __exit__(self, *_: Any) -> None:
        self.fechar()

    def __del__(self) -> None:
        try:
            self.fechar()
        except Exception:  # noqa: BLE001 - destrutor nunca propaga
            pass


def carregar(arquivo: str, binario: Optional[str] = None) -> Tilt:
    """Atalho para ``Tilt(arquivo)``."""
    return Tilt(arquivo, binario)


def chamar(arquivo: str, funcao: str, *args: Any, binario: Optional[str] = None, **nomeados: Any) -> Any:
    """Chama uma funcao uma unica vez (sobe e derruba o processo)."""
    with Tilt(arquivo, binario) as prog:
        return prog.chamar(funcao, *args, **nomeados)
