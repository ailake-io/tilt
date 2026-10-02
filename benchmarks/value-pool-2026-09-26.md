# Pool de ValueMap e ValueList — 26/09/2026

Comparação local no WSL Ubuntu 24.04, 100.000 linhas/objetos, 3 processos por variante e caso,
7 amostras por processo após uma execução de aquecimento (21 amostras por variante).
A ordem das variantes alterna entre rodadas. Cada processo executa apenas um caso.

| Caso | make_shared (ms) | pool (ms) | Variação de tempo | RSS anterior/pool (KiB) |
|---|---:|---:|---:|---:|
| materialize | 5.781 | 6.973 | +20.6% | 53344/53700 |
| join | 116.782 | 72.906 | -37.6% | 190380/191408 |
| lists | 4.629 | 5.703 | +23.2% | 45228/45532 |

## Método e limites

- As duas variantes compilam o mesmo value.cpp com -O3 -DNDEBUG; a referência troca somente o pool por std::make_shared e remove o recurso global. O código de produção não é alterado.
- Ambas usam a mesma libtilt_core.a, atualizada antes da medição. A unidade value.cpp é ligada antes do arquivo estático para fornecer os construtores correspondentes.
- Materialização: duas colunas inteiras, sem usar o cache de linhas. Join: tabelas de linhas, chave inteira única, correspondência 1:1. Listas: 100.000 listas de dois inteiros.
- Cada amostra valida todas as linhas/elementos, inclusive a coluna direita do join. Checksum esperado: 9.999.900.000.
- Tempos são medianas da construção/operação; preparação de entradas, validação e destruição do resultado ficam fora do cronômetro. Desalocações ainda ocorrem entre amostras, permitindo reutilização.
- RSS é o pico do processo inteiro, incluindo entradas, aquecimento e validação; não é memória exclusiva do pool nem memória liberada.
- Teste de uma thread e carga sintética. Não representa contenção concorrente, strings, chaves repetidas, joins colunares ou pipelines completos.
- Há amostras iniciais mais lentas mesmo após o aquecimento; os dados brutos foram preservados no JSON. Os números não devem ser tratados como garantia de desempenho.

O pool beneficiou o join deste teste, mas regrediu materialização e listas, sem redução do pico de memória.
Antes de alterar a política global, investigar a regressão e medir o ciclo completo de criação/destruição e cargas concorrentes.

## Reprodução

```bash
python3 scripts/benchmark_value_pool.py > benchmarks/value-pool-local.json
python3 scripts/benchmark_value_pool.py --rows 1000 --repeats 2 --rounds 1
```

Ambiente: Linux-6.18.33.2-microsoft-standard-WSL2-x86_64-with-glibc2.39
Compilador: c++ (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0
Base Git (checkout contém alterações anteriores): 284235d290fe6ac6cf18baf62f90fccd2f2dd7f2
