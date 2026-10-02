#!/usr/bin/env python3
"""Benchmarks de regressao (guia 16): roda os casos de bench/, normaliza pelo tempo de
uma calibracao local e compara com um baseline do mesmo ambiente.

  bench/comparar.py <tilt> [--baseline bench/baseline.json] [--atualizar]
                    [--tolerancia 1.6] [--repeticoes 3]

Compara `tempo / calibracao` apenas se CPU, arquitetura, sistema e versao do
Python coincidirem. Baselines antigos sem procedencia exibem so os tempos.
Falha (exit 1) se algum caso comparavel exceder a tolerancia.
"""
import argparse
import json
import os
import platform
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

AQUI = Path(__file__).resolve().parent

# (nome, argumentos do tilt, arquivo)
CASOS = [
    ("laco_3M", ["executar"], "laco.tilt"),
    ("laco_3M_vm", ["executar", "--vm"], "laco.tilt"),
    ("laco_3M_jit", ["executar", "--jit"], "laco.tilt"),
    ("fib_30", ["executar"], "fib.tilt"),
    ("csv_agrupar_parquet_1M", ["executar"], "dados.tilt"),
    ("csv_ordenar_1M", ["executar"], "ordenar.tilt"),
    ("parquet_ida_e_volta_1M", ["executar"], "parquet.tilt"),
]


def cronometrar(cmd, cwd, repeticoes):
    melhor = None
    for _ in range(repeticoes):
        inicio = time.perf_counter()
        r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True,
                           env={**os.environ, "TILT_VM_NOCACHE": "1", "LC_ALL": "C"})
        dur = time.perf_counter() - inicio
        if r.returncode != 0:
            sys.exit(f"falhou: {' '.join(cmd)}\n{r.stdout}{r.stderr}")
        melhor = dur if melhor is None else min(melhor, dur)
    return melhor


def calibracao(repeticoes):
    """Trabalho fixo de CPU (Python puro): mede a velocidade desta maquina."""
    codigo = "t = 0\nfor i in range(3000000):\n    t += i * 2\n"
    return cronometrar([sys.executable, "-c", codigo], None, repeticoes)


def ambiente():
    """Identifica a CPU e o Python usados pela calibracao do baseline."""
    cpu = platform.processor()
    info = Path("/proc/cpuinfo")
    if info.exists():
        for line in info.read_text(errors="replace").splitlines():
            if line.startswith("model name"):
                cpu = line.partition(":")[2].strip()
                break
    return {"sistema": platform.system(), "arquitetura": platform.machine(),
            "cpu": cpu, "python": platform.python_version()}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("tilt")
    ap.add_argument("--baseline", default=str(AQUI / "baseline.json"))
    ap.add_argument("--atualizar", action="store_true", help="grava o baseline em vez de comparar")
    ap.add_argument("--tolerancia", type=float, default=1.6)
    ap.add_argument("--repeticoes", type=int, default=3)
    args = ap.parse_args()
    tilt = str(Path(args.tilt).resolve())

    with tempfile.TemporaryDirectory() as tmp:
        for f in AQUI.glob("*.tilt"):
            shutil.copy(f, tmp)
        if not (AQUI / "vendas.csv").exists():  # ignorado pelo git; gerado uma vez
            subprocess.run([sys.executable, str(AQUI / "gerar_csv.py")], check=True,
                           capture_output=True)
        shutil.copy(AQUI / "vendas.csv", tmp)
        calib = calibracao(args.repeticoes)
        atual = {"ambiente": ambiente(), "calibracao_s": round(calib, 4), "casos": {}}
        for nome, modo, arquivo in CASOS:
            t = cronometrar([tilt, *modo, arquivo], tmp, args.repeticoes)
            atual["casos"][nome] = {"s": round(t, 4), "relativo": round(t / calib, 4)}

    print(f"calibracao: {calib:.3f}s")
    base = None
    if not args.atualizar and Path(args.baseline).exists():
        base = json.loads(Path(args.baseline).read_text())
    comparavel = base is not None and base.get("ambiente") == atual["ambiente"]
    if base is not None and not comparavel:
        print("aviso: baseline sem procedencia ou de outro ambiente; tempos exibidos "
              "sem classificar regressoes. Gere um baseline local com --atualizar.")
    falhas = 0
    for nome, r in atual["casos"].items():
        linha = f"{nome:28s} {r['s']:7.3f}s  rel {r['relativo']:6.3f}"
        if comparavel and nome in base["casos"]:
            ref = base["casos"][nome]["relativo"]
            razao = r["relativo"] / ref if ref > 0 else 1.0
            marca = "REGRESSAO" if razao > args.tolerancia else "ok"
            falhas += marca == "REGRESSAO"
            linha += f"  ref {ref:6.3f}  x{razao:4.2f}  {marca}"
        print(linha)
    if args.atualizar:
        Path(args.baseline).write_text(json.dumps(atual, indent=2) + "\n")
        print(f"baseline gravado em {args.baseline}")
    elif base is None:
        print("(sem baseline: rode com --atualizar para criar um)")
    sys.exit(1 if falhas else 0)


if __name__ == "__main__":
    main()
