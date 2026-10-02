# Benchmark após seleção tardia no filtro colunar

Comando:

```text
/tmp/tilt-bench-venv/bin/python scripts/benchmark_data_backends.py build/bin/tilt --rows 2000000 --repetitions 3 --json
```

Medianas em milissegundos na rodada:

| backend | group | filter | join |
| --- | ---: | ---: | ---: |
| Tilt | 168,897 | 186,852 | 374,296 |
| Tilt RPC | 160,136 | 146,737 | 341,567 |
| pandas | 449,339 | 430,695 | 569,882 |
| Polars | 41,014 | 41,601 | 51,317 |
| DuckDB | 105,744 | 100,910 | 100,954 |

O filtro agora devolve uma visão que compartilha as colunas e guarda somente os
índices selecionados. O agrupamento seguinte consome essa seleção sem copiar
as colunas. A variação entre rodadas ainda é relevante; os números servem como
referência para novas repetições.
