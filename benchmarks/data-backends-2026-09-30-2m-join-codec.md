# Benchmark após chaves compactas no join

Comando:

```text
/tmp/tilt-bench-venv/bin/python scripts/benchmark_data_backends.py build/bin/tilt --rows 2000000 --repetitions 3 --json
```

Ambiente e dados são os mesmos do relatório de referência. Medianas em
milissegundos:

| backend | group | filter | join |
| --- | ---: | ---: | ---: |
| Tilt | 192,951 | 193,659 | 370,000 |
| Tilt RPC | 163,770 | 169,239 | 344,111 |
| pandas | 384,882 | 384,216 | 527,003 |
| Polars | 43,080 | 39,495 | 58,384 |
| DuckDB | 91,634 | 89,272 | 91,049 |

Uma rodada mediu 344,111 ms no join RPC, contra 385,664 ms no relatório
anterior. Uma repetição posterior no mesmo ambiente mediu 366,825 ms, portanto
o ganho ainda não é estatisticamente confirmado e a variação do processo é
relevante. O caminho agora usa códigos de 32 bits para textos dictionary
encoded, evitando copiar o texto completo para a chave hash de cada linha. A
decisão final será feita com mais repetições após o executor por lotes.
