# Parquet: 2 milhões de linhas

Comando usado:

```text
/tmp/tilt-bench-venv/bin/python scripts/benchmark_parquet_large.py build/bin/tilt --rows 2000000 --row-group-size 20000 --repetitions 1
```

A carga tem uma lista e um struct nested, três codecs e leitura colunar com e sem scratch reutilizável. Os valores abaixo são da leitura colunar; RSS é o pico do processo em KiB e o último campo é o pico do scratch por row group.

| codec | modo | tempo (ms) | RSS (KiB) | scratch (bytes) |
| --- | --- | ---: | ---: | ---: |
| gzip | baseline | 934,887 | 235364 | 2699399 |
| gzip | scratch | 983,757 | 236396 | 2699399 |
| snappy | baseline | 967,059 | 246136 | 2660918 |
| snappy | scratch | 981,644 | 250188 | 2660918 |
| zstd | baseline | 849,827 | 233236 | 2660918 |
| zstd | scratch | 848,795 | 237108 | 2660918 |

A validação de roundtrip comparou todas as colunas com PyArrow. O caminho de struct plano evita a remontagem geral em `Value`; estruturas opcionais ou com listas continuam no decodificador nested geral.
