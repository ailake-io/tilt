# Parquet em carga grande — repetição de 29/09/2026

Medição com 1.000.000 de linhas, 100 row groups de 10.000 linhas e colunas
`id`, `list<int64>` e `struct{bucket:int64,label:string}`. Cada caso foi
executado uma vez em processo novo no binário Release, com PyArrow 21.0.0.

| Codec | Caminho | Operação | Baseline ms | Scratch ms | Delta tempo | RSS baseline | RSS scratch |
|---|---|---|---:|---:|---:|---:|---:|
| gzip | linhas | leitura | 1.042,1 | 1.151,0 | +10,5% | 1.242.500 KiB | 1.243.752 KiB |
| gzip | linhas | leitura+escrita | 1.598,4 | 1.898,1 | +18,8% | 1.248.992 KiB | 1.250.496 KiB |
| gzip | colunar | leitura | 461,7 | 449,7 | -2,6% | 125.880 KiB | 123.196 KiB |
| gzip | colunar | leitura+escrita | 1.319,3 | 1.268,5 | -3,9% | 129.392 KiB | 128.828 KiB |
| snappy | linhas | leitura | 982,2 | 1.032,7 | +5,1% | 1.247.728 KiB | 1.249.292 KiB |
| snappy | linhas | leitura+escrita | 2.373,9 | 1.648,1 | -30,6% | 1.308.248 KiB | 1.309.632 KiB |
| snappy | colunar | leitura | 523,9 | 465,8 | -11,1% | 130.392 KiB | 133.012 KiB |
| snappy | colunar | leitura+escrita | 1.355,1 | 1.337,4 | -1,3% | 187.996 KiB | 188.832 KiB |
| zstd | linhas | leitura | 915,3 | 1.184,5 | +29,4% | 1.241.376 KiB | 1.242.860 KiB |
| zstd | linhas | leitura+escrita | 2.477,5 | 1.527,2 | -38,4% | 1.245.856 KiB | 1.248.028 KiB |
| zstd | colunar | leitura | 518,2 | 449,4 | -13,3% | 126.748 KiB | 126.688 KiB |
| zstd | colunar | leitura+escrita | 1.243,7 | 1.183,7 | -4,8% | 126.748 KiB | 126.460 KiB |

Os arquivos de entrada tiveram 9,50 MiB (gzip), 15,43 MiB (Snappy) e 8,49 MiB
(Zstd). O pico interno por row group ficou em 1.542.475 bytes (gzip) e
1.490.477 bytes (Snappy/Zstd). O resultado confirma que o scratch é útil no
caminho colunar, com RSS baixo e até 13,3% de redução no tempo de leitura,
mas não justifica um allocator dedicado adicional. O caminho de linhas tem
variação alta entre processos e deve ser medido com mais repetições antes de
qualquer decisão.

Reprodução:

```bash
python3 scripts/benchmark_parquet_large.py build/release/bin/tilt \
  --rows 1000000 --row-group-size 10000 --repetitions 1 \
  > benchmarks/parquet-large-2026-09-29.json
```
