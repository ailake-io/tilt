# Listas: tamanho, concorrência e transferência — 26/09/2026

WSL Ubuntu 24.04, Ryzen 5 5600GT, GCC 13.3, C++20 -O3.
4 rodadas em ordem alternada; 2 aquecimentos e 5 amostras por processo.

| Elementos/threads/liberação | Listas/thread | Pool (ms) | Padrão (ms) | Variação padrão/pool |
|---|---:|---:|---:|---:|
| 0/1/local | 30000 | 1.468 | 0.927 | -36.8% |
| 0/2/local | 30000 | 4.994 | 1.760 | -64.8% |
| 0/2/handoff | 30000 | 13.388 | 1.700 | -87.3% |
| 0/4/local | 30000 | 14.093 | 2.561 | -81.8% |
| 0/4/handoff | 30000 | 39.324 | 2.560 | -93.5% |
| 2/1/local | 30000 | 6.228 | 2.953 | -52.6% |
| 2/2/local | 30000 | 8.659 | 4.466 | -48.4% |
| 2/2/handoff | 30000 | 19.289 | 4.643 | -75.9% |
| 2/4/local | 30000 | 14.367 | 7.648 | -46.8% |
| 2/4/handoff | 30000 | 51.927 | 6.836 | -86.8% |
| 32/1/local | 8192 | 11.018 | 4.933 | -55.2% |
| 32/2/local | 8192 | 13.005 | 6.744 | -48.1% |
| 32/2/handoff | 8192 | 14.489 | 6.798 | -53.1% |
| 32/4/local | 8192 | 18.541 | 13.004 | -29.9% |
| 32/4/handoff | 8192 | 43.166 | 12.408 | -71.3% |
| 256/1/local | 1024 | 10.177 | 4.109 | -59.6% |
| 256/2/local | 1024 | 11.804 | 6.058 | -48.7% |
| 256/2/handoff | 1024 | 12.183 | 5.972 | -51.0% |
| 256/4/local | 1024 | 16.245 | 11.299 | -30.4% |
| 256/4/handoff | 1024 | 22.451 | 10.987 | -51.1% |

## Método e limites

- Somente Value::lista alterna entre allocate_shared com o pool sincronizado e make_shared. Os mapas, tabelas e demais fontes são idênticos nas duas variantes.
- Listas por thread: mínimo de 30 mil e 262144/tamanho; listas vazias também usam 30 mil. Assim, comparações de tamanhos diferentes não representam a mesma quantidade de objetos.
- Workers persistem durante aquecimentos e amostras. Barreiras separam criação, validação e destruição. No modo handoff, cada worker consome as listas do vizinho; a thread produtora permanece viva.
- Todas as listas e todos os valores são verificados em cada amostra. Cada slot tem um único criador e um único destruidor em fases separadas.
- Por amostra, o ciclo soma o maior tempo de criação e o maior tempo de destruição entre workers. A mediana resume 20 amostras por variante; não é a soma das medianas nem throughput fim a fim.
- Validação e barreiras ficam fora do cronômetro; a validação pode aquecer caches antes da liberação. Entradas são inteiros; não cobre strings, grafos de objetos nem produtores que terminam antes da liberação.
- O vetor externo é reutilizado; os objetos ValueList e seus buffers internos são destruídos a cada rodada. Pico RSS inclui o processo inteiro.
- Resultados pequenos são sensíveis a ruído do sistema. Dados brutos, tempos separados e memória estão no JSON.

## Reprodução

```bash
python3 scripts/benchmark_list_handoff.py > benchmarks/list-handoff-local.json
python3 scripts/benchmark_list_handoff.py --max-lists 100 --max-elements 1024 --rounds 1 --repeats 2
```

## Decisão

Adotar `std::make_shared<ValueList>` em `Value::lista`: a variante padrão reduziu
o ciclo em todos os 20 cenários medidos, de 29,9% a 93,5%. O resultado vale para
as cargas e o ambiente descritos; não é uma garantia universal. Mapas e o
contêiner de linhas das tabelas permanecem com a política anterior.
