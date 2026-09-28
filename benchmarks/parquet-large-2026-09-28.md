# Parquet em carga grande: scratch por página/row group

Medição local com 1.000.000 de linhas, 100 row groups de 10.000 linhas e
colunas `id`, `list<int64>` e `struct{bucket:int64,label:string}`. Cada caso foi
executado uma vez em processo novo. O baseline usa `TILT_PARQUET_SCRATCH=0`,
recriando os vetores de repetition/definition levels e índices dictionary por
página; o caminho otimizado reutiliza o scratch do row group. A regravação foi
comparada integralmente com PyArrow, incluindo valores e nulos.

| Codec | Caminho | Operação | Baseline ms | Scratch ms | Delta tempo | RSS baseline | RSS scratch | Delta RSS |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| gzip | linhas | leitura | 876,5 | 821,6 | -6,3% | 1.213,5 MiB | 1.214,8 MiB | +0,1% |
| gzip | linhas | leitura+escrita | 1.446,8 | 1.424,3 | -1,6% | 1.220,2 MiB | 1.221,7 MiB | +0,1% |
| gzip | colunar | leitura | 442,6 | 443,3 | +0,1% | 123,3 MiB | 120,5 MiB | -2,3% |
| gzip | colunar | leitura+escrita | 1.226,4 | 1.257,0 | +2,5% | 127,0 MiB | 125,1 MiB | -1,5% |
| snappy | linhas | leitura | 890,1 | 846,7 | -4,9% | 1.219,0 MiB | 1.220,5 MiB | +0,1% |
| snappy | linhas | leitura+escrita | 1.548,8 | 1.502,3 | -3,0% | 1.277,7 MiB | 1.279,5 MiB | +0,1% |
| snappy | colunar | leitura | 476,6 | 465,0 | -2,4% | 127,5 MiB | 130,3 MiB | +2,2% |
| snappy | colunar | leitura+escrita | 1.267,1 | 1.307,2 | +3,2% | 183,6 MiB | 184,6 MiB | +0,6% |
| zstd | linhas | leitura | 805,5 | 796,3 | -1,1% | 1.212,5 MiB | 1.214,0 MiB | +0,1% |
| zstd | linhas | leitura+escrita | 1.377,2 | 1.364,2 | -0,9% | 1.217,3 MiB | 1.218,8 MiB | +0,1% |
| zstd | colunar | leitura | 431,4 | 432,4 | +0,2% | 123,8 MiB | 123,8 MiB | 0,0% |
| zstd | colunar | leitura+escrita | 1.138,6 | 1.160,5 | +1,9% | 123,8 MiB | 123,6 MiB | -0,2% |

O pico interno `pico_row_group_bytes` ficou estável após a métrica passar a
contar também os buffers temporários do baseline: aproximadamente 1,49–1,54 MiB
no caminho nested desta carga. Os arquivos e resultados brutos estão em
[`parquet-large-2026-09-28.json`](parquet-large-2026-09-28.json); reproduza com:

```bash
python3 scripts/benchmark_parquet_large.py build/release/bin/tilt \
  --rows 1000000 --row-group-size 10000 --repetitions 1 \
  > benchmarks/parquet-large-2026-09-28.json
```

As diferenças de tempo ficaram próximas do ruído e não há redução consistente
de RSS. O scratch reutilizável deve permanecer, porque reduz reservas e mantém
o pico controlado, mas os dados não justificam um allocator dedicado adicional
por row group neste momento. O próximo ganho provável está na escrita nested por
lotes, que ainda materializa estruturas intermediárias durante o roundtrip.
