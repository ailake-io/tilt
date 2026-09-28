# Pool: criação, destruição e concorrência — 26/09/2026

WSL Ubuntu 24.04; AMD Ryzen 5 5600GT, 6 núcleos/12 CPUs lógicas; GCC 13.3, -O3 -DNDEBUG.
100.000 linhas/objetos por thread; 1, 2 e 4 threads persistentes por processo.
4 rodadas com ordem alternada das variantes, 2 aquecimentos e 7 amostras por thread/rodada.

| Caso/threads | make_shared ciclo (ms) | pool ciclo (ms) | Variação | pool sem sincronização (ms) |
|---|---:|---:|---:|---:|
| materialize/1 | 11.487 | 25.690 | +123.6% | 24.399 |
| materialize/2 | 15.441 | 33.173 | +114.8% | não executado |
| materialize/4 | 28.249 | 47.579 | +68.4% | não executado |
| join/1 | 89.880 | 72.805 | -19.0% | 70.846 |
| join/2 | 112.437 | 100.582 | -10.5% | não executado |
| join/4 | 145.160 | 139.180 | -4.1% | não executado |
| lists/1 | 9.966 | 9.093 | -8.8% | 8.368 |
| lists/2 | 13.723 | 18.609 | +35.6% | não executado |
| lists/4 | 25.980 | 37.437 | +44.1% | não executado |

## Método

- Criação e destruição são cronometradas separadamente; ciclo é a soma por amostra. Validação completa entre as fases fica fora do tempo, mas pode aquecer caches antes da destruição. Não é uma medição ininterrupta de latência fim a fim.
- Tempos são medianas por worker, não throughput agregado. A carga total cresce com o número de threads: 100 mil, 200 mil e 400 mil objetos.
- Threads são criadas antes das medições e sincronizam o início de cada amostra com uma barreira. Entradas são independentes por worker; todas as threads compartilham o pool global sincronizado.
- A variante unsynchronized é apenas diagnóstica e nunca executada com múltiplas threads. Não é uma alternativa segura ao pool global sincronizado para produção.
- Mesmo o caso de uma thread usa um worker: o processo já ativou o modo multithread da libc. Por isso, não comparar diretamente os tempos absolutos com o relatório anterior, executado na thread principal sem criar workers.
- Cada amostra valida tamanho, todos os valores e checksum; joins validam também a coluna direita. Destruição ocorre na mesma thread da criação. Não cobre transferência entre threads.
- As três variantes compilam value.cpp a partir da mesma fonte e usam o mesmo arquivo estático do runtime. Nenhuma política de produção foi alterada para medir.
- Pico RSS é por processo e inclui entradas, todas as threads, aquecimento e validação. Dados brutos e tempos de cada fase estão em value-pool-lifecycle-2026-09-26.json.

## Reprodução

```bash
python3 scripts/benchmark_value_pool.py > benchmarks/value-pool-lifecycle-local.json
python3 scripts/benchmark_value_pool.py --rows 1000 --repeats 2 --rounds 1
```
