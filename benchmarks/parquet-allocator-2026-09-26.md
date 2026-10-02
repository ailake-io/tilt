# Parquet aninhado: validação das políticas de alocação — 26/09/2026

1,000,000 linhas; 100 row groups; arquivo gzip de 9.06 MiB; dados Arrow de 54.93 MiB.

| Caminho | Antes (ms) | Depois (ms) | Tempo | Pico RSS mediano antes/depois (MiB) |
|---|---:|---:|---:|---:|
| rows/read | 2042.7 | 2066.2 | +1.1% | 1825.2/1825.1 |
| rows/roundtrip | 3331.0 | 3313.7 | -0.5% | 1826.7/1826.4 |
| columnar/read | 534.3 | 512.1 | -4.2% | 83.7/83.6 |
| columnar/roundtrip | 2521.1 | 2484.6 | -1.4% | 1262.0/1261.7 |

## Método e limites

- Antes: pool nas listas e nos mapas materializados. Depois: make_shared nesses dois caminhos. Mapas gerais e contêineres de tabelas mantêm o pool.
- As duas versões compilam value.cpp e columnar.cpp com os mesmos flags e são ligadas ao mesmo runtime e main. A referência é uma reconstrução controlada dessas duas políticas anteriores, não um checkout histórico completo.
- Dados determinísticos: id inteiro, lista de inteiros com elemento nulo, listas vazias e nulas, struct com inteiro/texto e campos nulos. Geração por lotes reduz memória do gerador.
- Leitura isolada e pipeline leitura+regravação Parquet, ambos em linhas e colunar. Tempo inclui inicialização e encerramento do processo. Geração e validação externa ficam fora do cronômetro.
- Cada variante faz um aquecimento por caminho, seguido de quatro amostras em ordem alternada. O cache de arquivos do sistema não é limpo; resultados representam cache aquecido.
- A leitura isolada verifica contagem. Cada regravação é comparada integralmente por coluna com a entrada por PyArrow, incluindo ordem, tipos, todos os valores e nulos. Metadados e obrigatoriedade dos campos de topo podem diferir: o Tilt declara id obrigatório porque não há nulos. Não exige igualdade binária entre arquivos Parquet.
- Pico RSS é do processo Tilt medido por /usr/bin/time. O processo Python/PyArrow não entra no RSS, mas disputa recursos da mesma máquina.
- A carga é sintética e compressível, com múltiplos row groups. Não cobre todos os codecs, distribuições, armazenamento remoto ou concorrência de pipelines.
- O resultado combinado não separa o efeito de cada uma das duas mudanças. Os dados brutos incluem pico temporário de row group no caminho colunar.

## Reprodução

```bash
python scripts/benchmark_parquet_allocator.py --rows 1000000 --row-group-size 10000 --repetitions 4 > benchmarks/parquet-allocator-local.json
```

Requer Python com PyArrow, CMake, compilador C++20 e /usr/bin/time. Nesta máquina foi usado /home/tel/anaconda3/bin/python.

Ambiente: Linux-6.18.33.2-microsoft-standard-WSL2-x86_64-with-glibc2.39; PyArrow 21.0.0; c++ (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0.

## Conclusão e próximo gargalo

Os ganhos dos microbenchmarks não se traduziram em grandes ganhos ponta a ponta:
as medianas variaram entre -4,2% e +1,1%, com pico RSS praticamente estável.
São quatro amostras por variante; diferenças pequenas não comprovam ganhos
generalizáveis. A validação integral de todas as regravações passou.

A leitura colunar isolada atingiu cerca de 84 MiB; leitura+escrita atingiu
1262 MiB. O pico temporário de leitura por row group foi 2.427625 MiB
(2.545.551 bytes). Essa métrica não cobre todas as alocações do processo.
O arquivo de entrada comprimido tem apenas 9,06 MiB: este teste cobre um milhão
de linhas aninhadas, mas não arquivos de vários gigabytes no disco.

Em `src/runtime/parquet.cpp`, `table_to_columns` cria um vetor de `Value`
para todas as linhas de cada coluna (`column->at(row)`), e depois chama
`infer_column`. `parquet_write` converte a tabela inteira antes de codificar.
Esse caminho é um candidato concreto para explicar parte do pico; a fração
exata atribuível a ele exige profiling.

Priorizar escrita colunar em lotes/row groups, evitando intermediários de
tabela inteira, antes de introduzir outro alocador na leitura. A mudança do
gravador ainda não foi implementada nesta investigação.

Validação: smoke test de 1.000 linhas/5 row groups, benchmark de 1.000.000
linhas/100 row groups, comparação integral por coluna em todas as regravações,
compilação do script Python e `git diff --check`.
