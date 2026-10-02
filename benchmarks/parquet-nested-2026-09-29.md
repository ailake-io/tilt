# Materialização nested — 29/09/2026

O caminho colunar de Parquet foi ajustado para copiar folhas escalares de
structs por referência lógica e para eliminar o vetor temporário de `Value` ao
montar listas de structs. O decoder geral continua sendo usado quando os
níveis de repetição exigem remontagem.

| Linhas | Caminho | Mediana | RSS | Pico interno row group |
|---:|---|---:|---:|---:|
| 100.000 | linhas | 73,043 ms | 114.552 KiB | — |
| 100.000 | colunar | 27,066 ms | 16.272 KiB | 804.015 bytes |
| 1.000.000 | linhas | 745,647 ms | 1.053.030 KiB | — |
| 1.000.000 | colunar | 229,429 ms | 84.222 KiB | 804.015 bytes |

Na carga de 100 mil linhas, o caminho colunar ficou cerca de 12% abaixo da
medição anterior de 30,744 ms, com o mesmo resultado lógico e sem aumento do
pico de row group. A maior diferença continua sendo a representação colunar,
mas a remoção de cópias intermediárias reduz o custo da materialização nested.

Reprodução:

```bash
python3 scripts/benchmark_parquet_nested.py build/release/bin/tilt \
  --rows 100000 --repetitions 3
```
