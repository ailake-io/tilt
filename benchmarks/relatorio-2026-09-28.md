# Dados e adoção — 28/09/2026

Comando reproduzível:

```text
/tmp/tilt-bench-venv/bin/python scripts/benchmark_data_backends.py \
  build/release/bin/tilt --rows 1000000 --repetitions 3 --json
```

Ambiente local: build Release do Tilt, Python 3.13, pandas 3.0.6, Polars 1.44.2,
DuckDB 1.5.6 e PyArrow 25.0.1. A carga contém 1 milhão de vendas, cinco regiões
e uma tabela de dimensão com cinco linhas. Os resultados das três operações são
verificados antes da medição; Tilt inclui inicialização de processo, enquanto os
backends Python excluem importação.

| Backend | Agregação (ms) | Filtro + agregação (ms) | Join (ms) |
|---|---:|---:|---:|
| Tilt colunar | 261,209 | 261,216 | 342,003 |
| pandas | 231,980 | 228,538 | 306,835 |
| Polars lazy/streaming | 22,022 | 19,686 | 30,490 |
| DuckDB | 83,242 | 81,082 | 73,099 |

A matriz confirma que o caminho colunar do Tilt já fica próximo do pandas nessa
carga, mas ainda está atrás do DuckDB e principalmente do Polars. A diferença
continua incluindo o custo de iniciar o processo Tilt e não mede uma sessão
persistente; use o benchmark para comparar mudanças relativas no mesmo ambiente.
