# Benchmark de dados: 2 milhões de linhas

Comando usado:

```text
/tmp/tilt-bench-venv/bin/python scripts/benchmark_data_backends.py build/bin/tilt --rows 2000000 --repetitions 3 --json
```

Os tempos são medianas em milissegundos e incluem a leitura do CSV dentro de cada operação, como no benchmark anterior. Ambiente: WSL2 x86_64; pandas 3.0.6, Polars 1.44.2 e DuckDB 1.5.6. O processo persistente (`tilt_rpc`) exclui a inicialização do interpretador.

| backend | group | filter | join |
| --- | ---: | ---: | ---: |
| Tilt | 195,238 | 210,045 | 410,507 |
| Tilt RPC | 169,026 | 174,722 | 385,664 |
| pandas | 406,002 | 395,237 | 543,041 |
| Polars | 62,583 | 52,801 | 58,563 |
| DuckDB | 112,663 | 101,145 | 96,519 |

Neste cenário, o Tilt colunar ficou 2,08x mais rápido que pandas no agrupamento, 1,88x no filtro e 1,32x na junção. Polars e DuckDB ainda são mais rápidos nas três operações; os resultados são uma referência de otimização e não uma equivalência de plano físico entre os motores.
