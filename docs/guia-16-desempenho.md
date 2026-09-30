# 16 — Desempenho: onde estamos e como melhorar

O Tilt é uma linguagem **declarativa para dados**. Para pipelines, o custo dominante
costuma estar nos conectores, no Parquet e nas operações de tensor em C++, e aí o
desempenho é razoável. Para **lógica** escrita na própria linguagem (laços,
recursão, transformações linha a linha), o interpretador é lento. Este capítulo
registra medições reais, explica onde o tempo vai e propõe um plano, em ordem de
retorno, com como medir cada passo.

## Medições de referência

O [relatório de 24/09/2026](../benchmarks/relatorio-2026-09-24.md) registra
máquina, comandos, resultados atuais e comparação com CPython, pandas, NumPy
e CPU/GPU. Nesta máquina, o JIT executa o laço numérico em 117 ms contra 298 ms
do CPython, mas a recursão `fib(30)` ainda custa 265 ms contra 88 ms. O pipeline
CSV→agregação→Parquet de 1 milhão de linhas leva 804 ms no Tilt e 181 ms no
pandas. Em GEMM 1024², CUDA leva 3,19 ms (incluindo transferência) contra
9,33 ms na CPU com oneMKL; em 256² a CPU é mais rápida.

As tabelas históricas de `bench/baseline.json` foram medidas em outra máquina;
compare tempos absolutos e versões do ambiente antes de interpretar seus
limites normalizados como regressão.

## O que já foi feito

Principais otimizações disponíveis no runtime atual.

**Lógica (VM e interpretador)**
- Operadores pré-decodificados na VM (`BinOp`), aritmética inteira/decimal *in place* sem
  alocar `Value` temporário, escalares copiados campo a campo.
- Chamada de função VM → VM direta: quadro novo na mesma pilha, sem `Env`, sem mutex, sem
  vetor de argumentos por chamada; tabela de destinos por chunk (com cache do último).
- Recursão em modo JIT usa a chamada VM → VM quando o corpo não é compilável em JIT;
  assim `fib(30)` não passa pelo hook do interpretador em cada chamada.
- A VM funde padrões `local/literal/op`, `local/local/op` e `literal/local/op`
  em superinstruções, remapeando saltos antes de salvar o cache. Frames mantêm
  tags escalares paralelas aos valores para reduzir verificações no caminho quente.
- O JIT x86-64 compila chamadas para funções Tilt escalares, argumentos decimais e
  resultados decimais; chamadas que exigem builtins ou objetos continuam no fallback
  da VM. O JIT AArch64 usa o mesmo subconjunto em memória, com helpers nativos para
  chamadas, laços, impressão e operações escalares. O codegen nativo x86-64/ARM64
  também aceita as superinstruções.

**Dados**
- `ler_csv`: arquivo inteiro num buffer, varredura sem alocar por célula, cabeçalho
  resolvido uma vez e, acima de ~2 MB, leitura em até 8 threads (faixas alinhadas a `\n`).
- `ler_csv` e `ler_parquet` aceitam `selecionar: ["coluna", ...]`: o CSV evita
  materializar as células descartadas e o Parquet pula a descompressão e a
  decodificação das colunas não selecionadas.
- `ler_csv ..., inferir: verdadeiro` examina uma amostra limitada (`amostra:`)
  para fixar o tipo por coluna antes da leitura completa. Datas com hora podem
  ser normalizadas entre fusos IANA com `fuso:` e `destino_fuso:`.
- `perfil`, `inferir_schema`, `validar_schema` e `evoluir_schema` fornecem
  perfilagem automática e contratos JSON versionados para validar entradas e
  adicionar colunas compatíveis.
- `ler_csv` e `ler_parquet` com `colunar: verdadeiro` usam vetores tipados,
  dicionário de textos, offsets de listas e campos separados em estruturas estáveis.
  `agrupar_por` e filtros simples percorrem as colunas diretamente; a escrita
  Parquet evita mapas por linha. Outros métodos podem
  materializar linhas quando necessários. No pipeline de 1 milhão de linhas,
  esta opção reduziu a mediana de 646 para 187 ms e o pico de RAM de 513 para
  35 MiB no ambiente do relatório.
- `ler_csv`, `ler_parquet` e `ler_delta` aceitam `lazy: verdadeiro` (alias
  `preguicoso`). O retorno mantém um plano colunar e carrega a fonte somente no
  primeiro acesso; projeção, filtros e conversões ficam dentro do carregamento.
- `TILT_ANALYTIC_ENGINE=duckdb` (ou `TILT_DUCKDB_ANALYTICS=1`) delega agregações e
  junções colunares ao DuckDB quando a biblioteca e o appender estão disponíveis.
  `auto` delega apenas lotes a partir de 131.072 linhas; qualquer erro recua para
  o motor nativo. O padrão continua nativo.
- `paralelo: verdadeiro` no pipeline, ou `TILT_PIPELINE_PARALLEL=1`, executa em
  threads atribuições simples independentes. Dependências são detectadas pelos
  nomes lidos; etapas com dependência, escrita de arquivos ou saída textual permanecem sequenciais.
- Funções de usuário preservam a representação colunar dos argumentos; filtros
  numéricos simples com literal leem os vetores tipados sem criar um `Value`
  por linha. O RPC local Python tem transporte Parquet opt-in para evitar listas
  JSON em tabelas grandes (medição no relatório de 25/09/2026).
- `agrupar_por`, `filtrar`, `derivar`: sem cópias de `Value` por linha, um `Env` reaproveitado.
- `agrupar_por`: especificações de agregação são analisadas uma vez e os grupos
  acumulam soma/contagem/mínimo/máximo numa única passagem.
- `ordenar_por`: ordena índices sobre chaves extraídas uma vez (antes copiava a tabela e
  procurava a coluna a cada comparação); estável.
- Colunas de texto dictionary encoded ordenam por ranks lexicais pré-calculados,
  evitando comparar strings do dicionário em cada comparação do sort.
- Quando todas as chaves são inteiras e a ordem é crescente, `ordenar_por` aplica
  radix sort estável em cada chave, da última para a primeira.
- Para tabelas grandes, a fusão das partições ordenadas ocorre em árvore e em
  paralelo, evitando refazer a fusão de um prefixo crescente a cada partição.
- Agregações de uma única chave usam redução AVX2 para soma e soma de quadrados
  em colunas decimais quando disponível; CPUs sem AVX2 usam o caminho escalar.
- Quando a tabela está ordenada pela chave de agrupamento, as reduções são
  aplicadas diretamente aos intervalos contíguos de cada grupo.
- Mapas, listas e contêineres de linhas criados por `Value` usam um pool PMR
  compartilhado. O benchmark `scripts/benchmark_value_pool.py <tilt>` mede o
  custo de materialização e derivação, que são os caminhos com maior criação de
  objetos.
- Parquet: dicionário por `unordered_map` (teto de 1024 distintos), gzip no nível rápido,
  colunas geradas em paralelo na escrita (sem criptografia) e valores *movidos* na leitura.
- **`sql`** (e `tabela.sql`) roda SQL sobre tabelas em memória; com DuckDB lê CSV/Parquet
  direto, 6× mais rápido que `ler_csv` + `agrupar_por` (guia 03).

**Ferramentas**
- `bench/comparar.py` + `bench/baseline.json`: sete casos normalizados por uma calibração
  da máquina; baselines novos registram CPU e Python. Falha se algum ficar mais
  de 1,6× pior que a referência no mesmo ambiente. O baseline legado sem
  procedência só exibe os tempos, sem classificá-los como regressões.
- `scripts/benchmark_columnar_join.py <tilt>` compara join materializado e
  colunar em duas execuções consecutivas, incluindo o cache do índice hash. Em
  uma medição local de 20 mil linhas, o caminho colunar ficou 4,34× mais rápido;
  repita com `--rows` e `--repetitions` na máquina alvo.
- `scripts/benchmark_columnar_composite_join.py <tilt>` compara hash join e
  merge join colunar com duas chaves. O merge join só é escolhido quando os
  dois lados estão ordenados; o script mede os dois cenários separadamente.
  Após a comparação tipada sem serializar chaves, uma execução local com 20 mil
  linhas mediu 32,864 ms (merge) contra 32,878 ms (hash); repita na CPU alvo.
  Tabelas produzidas por `ordenar_por` carregam a ordem conhecida e não repetem
  a varredura de validação ao entrar no merge join.
  Em `tipo: "direita"`, o índice é mantido na tabela esquerda e pode ser
  reutilizado por chamadas seguintes com as mesmas chaves.
  Os postings do índice usam um vetor contínuo com buckets por offset, evitando
  uma alocação separada para cada chave repetida.
  Para inspecionar o cache durante a execução, use `t.metricas_join()`, que
  retorna `bytes`, `limite_bytes`, `buckets`, `posicoes`, `hits`, `misses` e
  `indices`. O limite padrão é 64 MiB por tabela; índices antigos são expulsos
  quando o limite é atingido.
  Para ajustar o orçamento, use `tabela = tabela.limitar_cache_join(1048576)`;
  o valor é informado em bytes e a redução remove índices antigos imediatamente.
  `tabela.metricas_memoria()` informa `bytes`, `pico_bytes`, `pico_row_group_bytes`,
  `linhas`, `colunas` e `cache_join_bytes`; o pico representa o maior uso observado
  desde a criação da tabela. Leitura Parquet de folhas escalares usa buffers diretos,
  portanto `pico_row_group_bytes` pode ser zero nesse caminho.
- `matmul` usa OpenBLAS/oneMKL por `dlopen` em matrizes 2D grandes (pelo menos
  1 milhão de produtos) quando há biblioteca CBLAS. `TILT_BLAS=off` força o
  kernel C++ portátil; `TILT_BLAS_LIBRARY=/caminho/libmkl_rt.so` escolhe uma
  instalação fora do caminho de bibliotecas do sistema. O binário não depende
  de BLAS para funcionar.
- A leitura Parquet de listas e structs reutiliza a capacidade dos buffers de
  valores e definition levels entre row groups; `clear()` remove apenas o
  tamanho lógico, reduzindo chamadas ao allocator.
- O decoder mantém um scratch por leitura e, no caminho nested, um scratch por
  row group para reutilizar payloads de páginas, cabeçalhos criptografados,
  repetition/definition levels e índices de dictionary entre folhas. As
  capacidades ficam limitadas à maior página decodificada.
- A rodada de 1 milhão de linhas com gzip, snappy e zstd confirmou que esse
  scratch mantém RSS e pico de row group estáveis, mas não produziu ganho
  consistente de tempo; LZ4_RAW e Brotli agora têm o mesmo caminho de scratch,
  e um allocator dedicado adicional fica adiado.
- A repetição de 29/09/2026 confirmou redução de 2,6% a 13,3% no tempo de
  leitura colunar com scratch, mantendo RSS entre 123 e 189 MiB; o caminho de
  linhas variou entre processos e ainda precisa de mais repetições. Os dados
  brutos estão em `benchmarks/parquet-large-2026-09-29.json`.
- O gravador Parquet escolhe entre gerar folhas em paralelo e anexá-las
  sequencialmente. Quando a capacidade estimada dos buffers passa de 256 MiB,
  cada corpo comprimido é anexado antes de gerar o próximo; isso reduz o pico
  de memória sem alterar schema ou encoding. Repita o benchmark de escrita na
  CPU alvo, pois o caminho sequencial troca memória por previsibilidade de RSS.
- Quando a entrada já é uma tabela colunar, folhas escalares são copiadas
  diretamente dos vetores tipados para o writer; listas, structs e colunas
  mistas continuam no caminho geral, que preserva a semântica de nulos e
  nested.
- Na leitura nested, structs agora anexam folhas escalares por referência e
  listas de structs não criam um vetor temporário de células por elemento.
  Em 100 mil linhas, a leitura colunar caiu para 27,066 ms e 16.272 KiB de RSS;
  o relatório está em `benchmarks/parquet-nested-2026-09-29.md`.
- `scripts/benchmark_parquet_nested.py <tilt>` mede tempo, RSS e
  `pico_row_group_bytes` para leituras nested em vários row groups.
  Na medição local atual com 100 mil linhas, o modo colunar levou 30,744 ms e
  17.856 KiB de RSS, contra 78,048 ms e 126.300 KiB no modo por linhas. A
  materialização nested usa até duas threads por leitura quando há vários row
  groups; o limite mantém o pico de memória previsível.
  Em um milhão de linhas, `benchmark_parquet_columnar.py` mediu 115,254 ms no
  caminho colunar, 631,331 ms por linhas e 86,599 ms no pandas; essa
  comparação inclui a criação de um processo Tilt por amostra e serve como
  baseline da máquina, não como garantia entre CPUs diferentes.
- O modo por linhas usa decodificação tipada direta para folhas escalares,
  materialização por grupo e até quatro workers para row groups grandes. Em um
  CSV→Parquet de 1 milhão de linhas com duas
  derivações aritméticas, `derivar` levou 0,32 s e o processo ficou em 70.236
  KiB de RSS.
- O caminho simples de `ler_csv` agora percorre spans diretamente no buffer e
  usa `from_chars` para inteiros e decimais, sem criar uma string por célula;
  aspas, nulos, tipos explícitos e projeções fora de ordem continuam no parser
  geral. Em `derivar`, expressões aritméticas entre colunas ou escalares usam
  `ColumnarColumn::binary_numeric`, com AVX2 no caminho denso.
- A ordenação de colunas inteiras usa radix sort estável tanto ascendente como
  descendente. Structs Parquet planos e obrigatórios decodificam cada folha
  diretamente no campo colunar, mantendo o decoder geral apenas para nested
  opcional ou com repetição.
- Em 2 milhões de linhas, `scripts/benchmark_columnar_large.py` mediu
  220,581 ms para `derivar`, 401,053 ms para ordenação e 200,187 ms para
  agregação. A matriz reproduzível contra pandas, Polars e DuckDB está em
  `benchmarks/data-backends-2026-09-30-2m.md`; a rodada Parquet com 2 milhões
  de linhas e três codecs está em `benchmarks/parquet-large-2026-09-30-2m.md`.
- `Value` passou de 120 para 112 bytes ao compartilhar o armazenamento dos
  escalares `logico`, `inteiro` e `decimal` em uma união. Na etapa seguinte,
  listas, mapas, tensores, funções e tabelas colunares foram reunidos em um
  único `ValueStorage`: o objeto agora mede 56 bytes. A string usa
  `CompactString` de 24 bytes com SSO para textos de até 22 bytes; textos
  maiores mantêm ownership próprio. Escalares não alocam
  esse bloco e os acessos internos usam referências tipadas (`list_ref`,
  `map_ref`, `tensor_ref` e `payload_ref`). O bloco reúne os slots de
  referência; as estruturas de listas e mapas continuam com ownership próprio.
- `scripts/benchmark_data_stack.py <tilt>` mede CSV → groupby → Parquet e
  escreve `backends_unavailable=...` quando Polars ou DuckDB não estão
  instalados; resultados ausentes não são tratados como zero.
- `scripts/benchmark_data_backends.py <tilt>` amplia a matriz para agregação,
  filtro + agregação e join em Tilt, pandas, Polars e DuckDB. Ele aceita `--json`
  e registra explicitamente os backends ausentes; um virtualenv pode instalar
  `polars` e `duckdb` sem alterar o ambiente do projeto.
- A comparação completa de 29/09/2026, com 1 milhão de linhas, mediu Tilt em
  sessão RPC persistente em 84,209/102,686/169,895 ms para agrupamento, filtro
  e junção; pandas em 169,915/167,889/195,806 ms; Polars em
  26,112/23,410/32,768 ms; e DuckDB em 83,735/78,854/71,633 ms. Os dados estão
  em `benchmarks/data-backends-2026-09-29.md`. A leitura CSV colunar direta
  removeu o vetor de `Value` por linha e reduziu o processo novo em 36%–51%.
- `conv2d` grande usa tiles im2col limitados em memória e CBLAS quando disponível;
  a implementação direta permanece para entradas pequenas ou CPU sem BLAS.
- CUDA e cuBLAS opcionais são carregados em tempo de execução. O caminho de
  alto nível sincroniza o `Tensor` com o host nas fronteiras sem kernel residente,
  enquanto a API `GpuBuffer`/`GpuGraph` mantém entradas, pesos e saídas no device
  entre operações. O backward denso usa esse caminho para os dois GEMMs e uma
  única redução do bias; operações pequenas continuam sujeitas ao custo de
  transferência quando não são agrupadas em um grafo.
- O cliente HTTP, inclusive chamadas LLM, usa libcurl nativa, com conexão e cache DNS por thread, se os
  headers de compilação e a biblioteca em runtime estiverem disponíveis.
  `TILT_HTTP_BACKEND=cli` força o caminho por subprocesso `curl`; a escolha
  automática recua para ele se libcurl não estiver disponível.
- Builds Release podem ativar LTO (`TILT_LTO=ON`) e PGO GCC
  (`TILT_PGO=GENERATE`/`USE` com `TILT_PGO_PROFILE_DIR`). O build padrão
  continua sem essas opções; instruções estão em [benchmarks](../benchmarks/README.md).

## Próximos passos

Em ordem de prioridade. O critério é o que mais pesa em **limpeza de dados e pipelines**;
o próximo salto da lógica pura depende agora de medir o efeito do `ValueStorage`
único em cargas de texto, listas e mapas.

1. ~~**API de limpeza de dados nativa**~~ feita (guia 03): `remover_nulos`,
   `preencher_nulos`, `renomear`, `remover_colunas`, `converter`, `deduplicar`, `juntar`,
   `empilhar`, `descrever`, `amostra`, `contar_valores`, `limpar_texto`. O que ainda falta
   nessa frente está em [guia 14](guia-14-roteiro.md).
2. ~~**`ler_parquet`**~~ feita: folhas escalares usam vetores tipados, row groups
   são materializados independentemente e a leitura por linhas pode usar workers;
   listas e structs nested são concatenados em lote, sem converter cada célula
   novamente em `Value`.
3. ~~**`derivar`**~~ feita para expressões aritméticas colunares simples: colunas
   existentes são copiadas por bloco, o literal é avaliado uma vez e a operação
   é executada diretamente nos vetores tipados, com SIMD quando disponível. A
   avaliação de expressões mais complexas ainda passa pela VM.
4. ~~**VM**~~ feita: superinstruções para operações locais e tags escalares paralelas
   no frame. Comparar o ganho em laços maiores continua recomendado antes de ampliar
   a fusão para chamadas e operações de objetos.
5. **DuckDB como motor opcional de `agrupar_por`/`juntar`** em tabelas grandes, com o mesmo
   resultado (ordem e tipos) do caminho nativo.
6. ~~**Compactação de `Value`**~~ feita em três etapas: a união dos escalares,
   o `ValueStorage` único e `CompactString` reduziram o objeto de 120 para
   56 bytes. Textos de até 22 bytes não alocam; textos maiores continuam com
   ownership independente para não aumentar o bloco compartilhado.
7. ~~**JIT dinâmico ARM64**~~ feito: `executar --jit` gera código AArch64 em
   memória para o subconjunto escalar e recua para a VM em estruturas e builtins
   não suportados.

## Por que a lógica é lenta

1. **`Value` ainda carrega uma string inline.** A união escalar, o storage de
   referências e `CompactString` já reduziram o objeto para 56 bytes, e textos
   curtos não alocam. Textos longos, listas e mapas ainda têm seus próprios objetos heap-backed;
   cópias desses valores continuam custando mais que a própria soma em laços
   quentes.
2. **Variáveis por nome.** `Env` guarda `unordered_map<string, Value>`; cada leitura
   ou escrita é um hash de texto, e cada iteração de `enquanto`/`para cada` cria um
   `Env` novo (alocação).
3. **Chamadas do interpretador caras.** Quando a função não passa pela VM,
   `call_function` consulta o bytecode e monta um `Env` por chamada.
4. **A VM não usa os tipos.** O checker já infere tipos, mas a VM emite as mesmas
   operações genéricas e checa a tag de cada operando em toda instrução.
5. **Tabela por linhas ainda é o padrão.** Uma `tabela` de 1 M linhas são 1 M
   `ValueMap` com chaves em texto e valores boxeados. O modo colunar do CSV
   evita esse custo para `agrupar_por`; outras operações ainda podem
   materializar os mapas.

## Plano de fundo: lógica (detalhe do item 6 acima)

1. **`Value` compacto (64 bytes).** A união escalar e o `ValueStorage` único já
   foram entregues e medidos em `tests/value_size_test.cpp`. O próximo ponto
   opcional é mover também a string para o storage, o que reduziria o objeto
   novamente, mas só deve ser feito após medir textos curtos e longos em
   `bench/laco.tilt` e `bench/fib.tilt`.
2. **Variáveis por slot.** Uma passada depois do checker atribui a cada variável um
   índice de quadro; `Env` vira um vetor. Fim do hash por acesso e da alocação por
   iteração. Esperado: 1,5–2× adicional.
3. **Chamadas baratas.** Guardar o chunk compilado na própria declaração da função
   (sem mapa nem mutex no caminho quente), reaproveitar o quadro e evitar copiar
   argumentos. Alvo: aproximar `fib(30)` do CPython (hoje 8× atrás).
4. **VM de registradores com tipos.** Usar os tipos do checker para emitir
   `soma_int`, `menor_int`, etc. sem checar a tag, e manter inteiros em registradores.
   Isso pode dar vantagem à VM em relação ao interpretador de árvore.
5. **JIT com chamadas e decimais**: o backend x86-64 e o AArch64 já cobrem o
   subconjunto escalar; falta ampliar a cobertura para objetos e builtins sem
   aumentar o custo de compilação.

## Plano de fundo: dados (parte já feita acima)

1. **Ampliar a tabela colunar.** CSV e Parquet já podem guardar `int64`, `double`,
   lógicos e texto por dicionário; `agrupar_por`, junções, filtros compostos,
   projeções nested e a escrita Parquet operam nas colunas. Folhas escalares e
   textos PLAIN são decodificados sem `Value` temporário por página; listas e
   structs são concatenados em lote.
   Medir com `scripts/benchmark_data_stack.py` e `/usr/bin/time -v`.
2. **Paralelismo.** A leitura Parquet já materializa row groups independentes em
   paralelo (com limite de duas threads no nested); a leitura CSV por faixas de
   bytes e o merge paralelo de operações gerais continuam como próximos ganhos.
3. ~~**Delegar ao DuckDB**~~ feita de forma opcional: agregações e junções colunares
   usam SQL gerado apenas quando o motor é solicitado ou o modo `auto` amortiza a carga.
4. ~~**Planos preguiçosos com pushdown**~~ feita para CSV, Parquet e Delta locais e
   conectores remotos. `lazy: verdadeiro` adia a primeira consulta; SQL mantém
   projeção/filtro/limite parametrizados e Elasticsearch/OpenSearch traduz
   `pushdown.colunas`, `pushdown.onde` e `pushdown.limite` para `_source`,
   `bool.filter` e `size`.
5. ~~**Rede e loops.**~~ O cliente HTTP persistente já reduz o custo das chamadas
   repetidas; fontes remotas podem ser lazy e aplicar pushdown. A execução paralela
   de passos e o loop independente `saida[i] = expressao` estão disponíveis com
   `TILT_PIPELINE_PARALLEL=1`/`TILT_LOOP_PARALLEL=1`; o runtime recua para serial
   quando detecta efeitos colaterais ou dependências.

## Como medir e não regredir

- `bench/rodar.sh [caminho-do-tilt]` roda os casos no interpretador, na VM e no JIT (gera
  `bench/vendas.csv` na primeira vez; o arquivo é ignorado pelo git).
- `bench/comparar.py <tilt>` compara com `bench/baseline.json` (`--atualizar` regrava, ao
  final de uma otimização confirmada); a comparação é por tempo / calibração da máquina.
- Compare *antes/depois na mesma máquina*, com o mesmo binário Release
  (`cmake --preset release`), e repita 3 vezes: os números variam com a carga.
- Regra de trabalho: **uma otimização por vez**, com o número antes e depois no
  commit; o interpretador tem ~12 mil linhas e vários pontos de acoplamento
  (`Value`, `Env`, VM, JIT, codegen), então mudanças de representação (`Value`,
  slots) merecem um branch próprio e a suíte completa (`ctest`) mais os *goldens*
  antes de mesclar. O backward CUDA de convolução, recorrência e embeddings já
  tem kernels dedicados; operadores fora deles e Metal continuam com fallback CPU.

## O que não é gargalo

Treino e inferência de tensores e conectores já rodam em código nativo; otimizar
o laço do interpretador não acelera seu núcleo numérico. Ainda há gargalos
próprios: materialização de linhas, operadores não cobertos por kernels e
transferência host↔GPU. Para modelos grandes, o ganho seletivo da GPU está
medido no [relatório](../benchmarks/relatorio-2026-09-24.md).
