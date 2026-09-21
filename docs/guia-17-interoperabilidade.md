# 17 — Interoperabilidade: Python, PySpark e Kof

O Tilt não pede que você abandone o que já usa. Há três caminhos, do mais simples
ao mais acoplado:

| Caminho | Quando usar | Como |
|---|---|---|
| **Arquivos** | pipelines em lote, Spark/Python lendo o que o Tilt escreve (e o inverso) | Parquet, Delta, Iceberg, CSV, JSON (guia 03) |
| **Chamada de função** | usar a lógica de um `.tilt` dentro de outro programa (ou o contrário) | `tilt rpc` (JSON-lines ou HTTP), pacote Python `tilt`, `chamar_python` |
| **Serviço** | outro time/linguagem consome o Tilt (ou o Tilt consome) por rede | `servico` (guia 07) ou `tilt rpc --porta`; `http_post_json` |

Tudo abaixo roda hoje e tem teste automatizado (`ctest -R "rpc|python|kof"`).
Os exemplos usam [`exemplos/interop/vendas.tilt`](../exemplos/interop/vendas.tilt).

## `tilt rpc`: funções e pipelines para fora

```bash
tilt rpc exemplos/interop/vendas.tilt                 # JSON-lines: stdin/stdout
tilt rpc exemplos/interop/vendas.tilt --porta 8090    # o mesmo por HTTP (127.0.0.1)
tilt chamar exemplos/interop/vendas.tilt classificar 120   # uma chamada, imprime JSON
```

Toda `funcao` de topo (sem `_` no início) e todo `pipeline` do arquivo ficam
chamáveis. O programa é checado antes de subir: se há erro de tipo, nada sobe.

**JSON-lines** — uma requisição JSON por linha no stdin, uma resposta por linha no
stdout. A primeira linha do stdout é um *banner* com `protocolo`, `versao`,
`funcoes` (nome e parâmetros) e `pipelines`.

| Requisição | Efeito |
|---|---|
| `{"id":1,"chamar":"soma","args":[1,2]}` | chama com argumentos posicionais |
| `{"chamar":"soma","args":[1],"nomeados":{"b":7}}` | nomeados entram pela posição do parâmetro |
| `{"chamar":"classificar","lote":[[10],[60]]}` | N chamadas em uma ida e volta; resultado é a lista |
| `{"pipeline":"demo"}` | roda um pipeline |
| `{"listar":true}` / `{"ping":true}` / `{"sair":true}` | banner / vivo? / encerra |

Resposta: `{"id":1,"ok":true,"resultado":3}` ou `{"id":1,"ok":false,"erro":"linha 25: ..."}`.
O que o programa imprimiu (`imprimir`) vai no campo `saida`, nunca solto no stdout.
Um erro de execução **não derruba** o processo: a próxima requisição funciona.

**HTTP** (`--porta N`, padrão `127.0.0.1`; `--host` para mudar):

| Rota | Corpo | Resposta |
|---|---|---|
| `GET /funcoes`, `GET /saude` | — | banner / `{"ok":true}` |
| `POST /chamar/<funcao>` | lista de args, ou `{"args":[...],"nomeados":{...}}` | como no JSON-lines |
| `POST /lote/<funcao>` | lista de listas de args | lista de resultados |
| `POST /pipeline/<nome>` | — | `saida` do pipeline |

Códigos: `200` ok, `400` erro de execução ou corpo inválido, `404` função ou rota
inexistente. **Sem autenticação**: fica em `127.0.0.1` por padrão; se expuser
com `--host 0.0.0.0`, ponha um proxy com autenticação na frente.

**Tipos.** JSON ↔ Tilt: `inteiro` ↔ inteiro, `decimal` ↔ número com ponto
(`2.0` continua decimal), `texto`, `logico`, `nulo`, `lista`, `mapa` ↔ objeto.
Uma `tabela` vira lista de objetos (e uma lista de objetos aceita `.filtrar`,
`.derivar`, `.agrupar_por`). Tensor vira `{"forma":[...],"dados":[...]}`;
NaN/Infinity viram `null`.

## Python

```bash
pip install ./python        # pacote `tilt-lang`, sem dependências; importa como `tilt`
```

Precisa do executável `tilt` no `PATH` (ou `TILT_BIN=/caminho/tilt`).

```python
import tilt

vendas = tilt.carregar("exemplos/interop/vendas.tilt")   # sobe `tilt rpc`
vendas.classificar(120)                                  # 'alto'
vendas.so_altos(linhas, minimo=20)                       # nomeados
vendas.chamar_lote("classificar", [[10], [60], [200]])   # 1 ida e volta
resultado, impresso = vendas.chamar_com_saida("saudar", "Ana")

import pandas as pd
df = pd.DataFrame(vendas.enriquecer(df))                 # DataFrame entra e sai
vendas.fechar()                                          # ou `with tilt.carregar(...) as v:`
```

`pandas.DataFrame`, tabelas `pyarrow` e arrays `numpy` são convertidos ao enviar.
Erros do Tilt viram `tilt.TiltErro` (com `.saida`). O objeto é seguro entre threads
(as chamadas são serializadas). Para uma chamada avulsa: `tilt.chamar(arq, "f", 1)`.

### Tilt chamando Python: `chamar_python`

```tilt skip
# modulo da stdlib do Python ou arquivo .py (procurado ao lado do programa)
pipeline limpar:
  passos:
    - imprimir chamar_python("math", "sqrt", 16)                       # 4.0
    - t = chamar_python "limpeza.py", "normalizar", tabela, escala: 2  # nomeados
    - imprimir chamar_python("numpy", "linalg.norm", [3, 4])           # funcao com ponto
```

Cada chamada roda `python3` num subprocesso; argumentos e resultado viajam como
JSON em arquivos temporários (nada é interpolado numa shell) e o `print` do
módulo vai para o stderr. Uma lista de objetos devolvida vira `tabela`. Erros do
Python chegam como erro capturável com `arquivo:linha`
(`tentar: ... capturar erro:`). Escolha o interpretador com `TILT_PYTHON` ou
`python: "/caminho/python"`. Como o processo sobe a cada chamada, prefira
funções que recebam a tabela inteira (não uma linha por vez).

## PySpark

**1. Arquivos.** O Tilt lê e escreve Parquet (snappy/gzip, listas), Delta e Iceberg,
inclusive particionados, e fala com Spark por Livy (`fonte tipo: spark`). Um pipeline
Tilt pode alimentar um job Spark, e o contrário, sem nenhum código de ligação:

```tilt skip
pipeline preparar:
  passos:
    - vendas = ler_csv "vendas.csv"
    - escrever_delta vendas, "lake/vendas", particionar_por: "regiao"
# no Spark:  spark.read.format("delta").load("lake/vendas")
```

**2. Lógica Tilt dentro do Spark.** `tilt.spark` roda funções `.tilt` em DataFrames:

```python
from tilt.spark import transformar, por_grupo, coluna

# lote a lote (mapInPandas): a funcao recebe e devolve uma tabela
ricos = transformar(df, "vendas.tilt", "enriquecer",
                    "regiao string, valor long, faixa string")

# grupo inteiro (groupBy.applyInPandas): use para agregacoes
totais = por_grupo(df, "regiao", "vendas.tilt", "total_por_regiao",
                   "regiao string, total long, pedidos long")

# coluna a coluna (pandas_udf), em lotes de uma ida e volta
df.withColumn("faixa", coluna("vendas.tilt", "classificar", "string")("valor"))
```

Cada partição sobe um processo `tilt rpc`. Num cluster, os executores precisam de
três coisas: o pacote `tilt` (`pip install`, ou `--py-files`), o executável `tilt`
(`TILT_BIN` ou `PATH`) e o `.tilt` (`--files vendas.tilt`; use `SparkFiles.get` para
o caminho). `transformar` vê só um lote por vez: para somar/contar de verdade use
`por_grupo` ou o próprio Spark. Validado com PySpark 4.2 em modo local
(`python/testes/test_spark.py`).

## Kof

[Kof](https://koflang.github.io/) roda em JVM, nativo e JS e tem cliente HTTP
(`kof.http`) e servidor (`kof serve`). Como HTTP é comum às duas linguagens, a ponte
é um endpoint — sem biblioteca especial.

**Kof → Tilt.** Suba `tilt rpc arquivo.tilt --porta 8090` e chame de Kof
(`exemplos/interop/kof/cliente.kf`):

```kof
import kof.http

record Regiao(String regiao, Int total, Int pedidos) {}
record RespRegioes(Bool ok, List<Regiao> resultado) {}

String tilt(String funcao, String args) {
    return http.post("http://127.0.0.1:8090/chamar/" + funcao, args, "Content-Type: application/json")
}

main() {
    var r = json.decode<RespRegioes>(tilt("total_por_regiao", "[[{\"regiao\":\"sul\",\"valor\":30}]]"))
    println(r.resultado.get(0).regiao + " " + r.resultado.get(0).total)   // sul 30
}
```

O `json.decode<T>` tipado do Kof mapeia direto na resposta. Um `servico` Tilt
(`tilt servir`) também serve, se você preferir rotas nomeadas.

**Tilt → Kof.** Um `kof serve` com `handle(...)` é só um endpoint para
`http_post_json` (`exemplos/interop/kof/chamar_kof.tilt`):

```tilt skip
pipeline pontuar:
  passos:
    - r = http_post_json "http://127.0.0.1:8091/pontuar", { cliente: "bia", valor: 250 }
    - imprimir r.cliente, r.pontos     # bia 25
```

O teste `kof_interop` sobe os dois lados com o toolchain `kof` real (e é pulado se
`kof`, `curl` ou `python3` não estiverem instalados).

## Limites conhecidos

- Cada processo `tilt rpc` atende uma requisição por vez (o interpretador é
  compartilhado); para paralelismo suba vários (o cliente Python por partição do
  Spark já faz isso).
- Sem autenticação nem TLS no `--porta`.
- Não há memória compartilhada nem Arrow entre processos: dados grandes vão por
  arquivo (Parquet), não por JSON.
- `chamar_python` sobe um interpretador por chamada (dezenas de ms). Para
  chamadas em laço use uma função Python que receba a lista inteira.
