# Comparação de backends de dados — 29/09/2026

Carga de 1.000.000 de linhas em CSV, cinco regiões e três operações. A medição
usa três repetições e a mediana; a geração dos dados fica fora do tempo. Tilt
executa o binário Release e os backends Python releem o CSV em cada amostra.

| Operação | Tilt processo (ms) | Tilt RPC persistente (ms) | pandas (ms) | Polars lazy (ms) | DuckDB (ms) |
|---|---:|---:|---:|---:|---:|
| Agrupamento | 120,281 | 84,209 | 169,915 | 26,112 | 83,735 |
| Filtro + agrupamento | 132,082 | 102,686 | 167,889 | 23,410 | 78,854 |
| Junção | 206,686 | 169,895 | 195,806 | 32,768 | 71,633 |

Depois da leitura CSV direta para colunas, o Tilt ficou 0,71x a 1,06x do tempo
de pandas no processo novo e 0,50x a 0,87x em RPC persistente. Em relação à
medição anterior, o processo novo reduziu 51,4% no agrupamento, 46,3% no filtro
e 36,4% na junção. A sessão persistente reduziu 56,7%, 50,6% e 38,0%,
respectivamente. DuckDB ainda é melhor na junção e Polars permanece muito à
frente, especialmente por operar com kernels altamente vetorizados.

O resultado muda a prioridade de otimização: o caminho simples de leitura CSV,
O vetor de `Value` por linha foi removido do caminho CSV colunar simples; cada
coluna agora recebe inteiros, decimais, textos e nulos diretamente. O próximo
alvo é eliminar materializações equivalentes em filtros e joins nested, sem
alterar o caminho geral para tipos complexos.
Delegar para DuckDB continua disponível de forma opt-in para cargas grandes.

Ambiente: Python 3.13, pandas 2.3.3, Polars 1.44.2, DuckDB 1.5.6 e PyArrow
21.0.0. Polars e DuckDB foram instalados apenas no virtualenv temporário
`/tmp/tilt-bench-venv`.

Dados brutos: [`data-backends-2026-09-29.json`](data-backends-2026-09-29.json).
