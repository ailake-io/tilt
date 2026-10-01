# Benchmark após agrupamento por códigos dictionary

Comando:

```text
/tmp/tilt-bench-venv/bin/python scripts/benchmark_data_backends.py build/bin/tilt --rows 2000000 --repetitions 3 --json
```

Medianas em milissegundos na última rodada:

| backend | group | filter | join |
| --- | ---: | ---: | ---: |
| Tilt | 168,139 | 194,171 | 377,760 |
| Tilt RPC | 134,493 | 160,630 | 352,879 |
| pandas | 431,536 | 431,078 | 608,800 |
| Polars | 43,787 | 42,316 | 53,301 |
| DuckDB | 97,749 | 91,251 | 88,953 |

O caminho paralelo de `agrupar_por` agora identifica os grupos de uma chave
`Text` diretamente pelo código dictionary, sem chamar `key_at()` e sem montar
uma chave textual por linha. A rodada anterior do mesmo benchmark marcou
169,026 ms no Tilt RPC para agrupamento; a diferença observada é de
aproximadamente 20%, mas deve ser confirmada com uma série maior antes de virar
um limite fixo de regressão.
