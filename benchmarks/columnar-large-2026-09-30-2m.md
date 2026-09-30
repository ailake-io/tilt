# Operações colunares em 2 milhões de linhas

Comando usado:

```text
python3 scripts/benchmark_columnar_large.py build/bin/tilt --rows 2000000 --repetitions 3
```

| operação | mediana (ms) |
| --- | ---: |
| `derivar` numérico | 220,581 |
| ordenação inteira descendente | 401,053 |
| agregação por grupo | 200,187 |

A derivação usa o caminho vetorizado de `ColumnarColumn::binary_numeric`, com SIMD quando AVX2 está disponível. A ordenação de chaves inteiras usa radix sort estável também no sentido descendente. A agregação permanece colunar e o resultado foi validado contra cinco grupos.
