# Cardinalidade e backends — 30/09/2026

Matriz com 2.000.000 de linhas e cardinalidade de `regiao` igual a 5, 5.000 e
500.000. Foram usadas três repetições nos casos de 5 e 5.000 grupos; o caso de
500.000 grupos usou uma repetição por ser limitado por memória. O benchmark inclui leitura CSV e a
operação; o Tilt RPC exclui o custo de inicialização. Polars 1.x, DuckDB 1.x e
pandas 2.x foram executados no virtualenv local `.venv-bench`.

## Agrupamento — Tilt RPC (ms)

| grupos | Tilt normal | Tilt spill | pandas | Polars | DuckDB |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 5 | 152,896 | 567,671 | 385,078 | 47,400 | 106,433 |
| 5.000 | 194,960 | 618,021 | 485,939 | 55,245 | 113,233 |
| 500.000 | 2.840,011 | 1.497,294 | 937,530 | 78,948 | 155,343 |

O spill é mais lento em baixa cardinalidade, mas reduziu o tempo do agrupamento
em 47,3% com 500 mil grupos. Na medição separada com `/usr/bin/time`, o mesmo
formato de carga caiu de 1.675.064 para 409.496 KiB de RSS (−75,6%).

## Filtro + agregação — Tilt RPC (ms)

| grupos | Tilt normal | Tilt spill | pandas | Polars | DuckDB |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 5 | 147,958 | 362,039 | 371,245 | 45,730 | 93,263 |
| 5.000 | 167,707 | 402,480 | 446,237 | 49,213 | 104,113 |
| 500.000 | 1.216,795 | 805,029 | 954,111 | 63,556 | 123,648 |

Os dados brutos estão em `cardinality-g*.json`; a matriz foi gerada por
`scripts/benchmark_data_backends.py --groups`.
