# Desempenho do Tilt — 24/09/2026

Medição local após as otimizações de CUDA/cuBLAS opcionais, CBLAS opcional na
CPU, agregação em passagem única e correção do fallback de recursão no JIT.
Build CMake Release com `TILT_WERROR=ON`, GCC 13.3, Linux x86-64, AMD Ryzen 5
5600GT (6 núcleos/12 threads), NVIDIA GeForce RTX 5050, Python 3.13.9,
pandas 2.3.3, NumPy 2.3.5 e pyarrow 21. PyTorch não estava instalado; não há
comparação medida com ele. Tempos são medianas, em milissegundos, de execuções
sequenciais na mesma máquina. As saídas foram conferidas pelos benchmarks.

## Lógica da linguagem

`python3 scripts/benchmark_language_core.py /tmp/tilt-build/bin/tilt --repetitions 5`
mede processos completos, inclusive inicialização e impressão. O algoritmo e
o resultado são equivalentes em Tilt e CPython.

| Operação | Tilt interpretador | Tilt VM | Tilt JIT | CPython |
|---|---:|---:|---:|---:|
| Laço inteiro, 3 milhões de passos | 346,5 | 343,4 | **117,2** | 297,7 |
| `fib(30)` recursivo | 265,2 | 264,2 | 265,4 | **87,8** |

O JIT é 2,54× mais rápido que CPython no laço coberto pelo compilador. O
`fib(30)` do Tilt ainda é ~3,0× mais lento. A correção recente eliminou uma
regressão: recursão não compilável em JIT antes custava ~907 ms por passar pelo
hook a cada chamada; agora usa a chamada direta VM→VM. A VM genérica quase não
difere do interpretador no laço. Próximo investimento: representação compacta
de `Value`, slots locais e chamadas especializadas; requer revisão ampla da
semântica e testes de paridade.

Uma compilação PGO GCC treinada com os dois casos e o pipeline de dados reduziu
`fib(30)` na VM de 263,0 para 207,2 ms (medianas de 3 execuções em rodada
posterior); o laço JIT ficou em 117,7 vs 116,3 ms. Trata-se de build opcional:
o ganho depende dos casos usados no treino e não substitui a compactação de
`Value`.

## Pipeline analítico

`python3 scripts/benchmark_data_stack.py /tmp/tilt-build/bin/tilt --repetitions 5`
gera CSV de 1 milhão de linhas, lê, agrupa e escreve Parquet gzip com o mesmo
resultado agregado. A mediana exclui importação do pandas; a execução Tilt
também usa processo separado. Portanto a comparação representa o pipeline
quente, não o tempo para carregar as bibliotecas Python.

| Pipeline | Tilt | pandas | Relação |
|---|---:|---:|---:|
| CSV → groupby → Parquet gzip, 1 M linhas | 803,7 | **181,1** | Tilt 4,44× mais lento |

Após adicionar `selecionar:` às fontes, uma rodada posterior de três repetições
na mesma máquina deu 813,1 ms para o pipeline Tilt completo e 714,1 ms lendo
só `regiao` e `valor` (12,2% menos); pandas passou de 191,5 para 142,8 ms
com `usecols`. O CSV projetado não constrói o texto das outras colunas; no
Parquet, as colunas descartadas não são descomprimidas nem decodificadas.
Os novos números são de outra rodada e não devem ser misturados com a tabela
original para inferir pequenas diferenças.

Com `ler_csv ..., selecionar: ["regiao", "valor"], colunar: verdadeiro`, a
leitura guarda números em vetores contíguos e codifica os cinco nomes de região
em um dicionário; `agrupar_por` lê essas colunas sem criar mapas para cada linha.
Em uma medição sequencial posterior (7 repetições), o pipeline completo levou
186,9 ms no Tilt colunar, 646,3 ms no Tilt por linhas com projeção, 168,4 ms
no pandas completo e 129,3 ms no pandas com `usecols`. O caminho colunar foi
3,46× mais rápido que o caminho projetado por linhas. Medido separadamente
com `/usr/bin/time`, o pico caiu de 512.928 KiB para 35.404 KiB (14,5× menos).
O modo colunar é opt-in: os demais métodos materializam as linhas sob demanda,
preservando a semântica da tabela quando necessário.

Para `ler_csv` → `filtrar linha.valor >= 250` → `agrupar_por` em 1 milhão de
linhas, as saídas dos dois modos foram idênticas (501.366 linhas filtradas).
Em cinco execuções sequenciais, a mediana caiu de 924,3 para 212,6 ms
(4,35×); o pico de memória caiu de 573.352 para 35.248 KiB. O filtro colunar
atual cobre comparações simples de uma coluna com um literal; expressões mais
complexas ainda materializam as linhas antes de filtrar.

Na exportação integral `ler_csv` projetado → `escrever_parquet` gzip, com
1 milhão de linhas e duas colunas, o caminho colunar levou 498 ms contra
825 ms por linhas (5 execuções, mediana). O pico caiu de 576.024 para
193.344 KiB; os dois arquivos Parquet foram lidos pelo pandas e comparados
linha a linha com igualdade exata. A escrita ainda usa `Value` temporários
por coluna durante a inferência do esquema, mas não cria mapas por linha.

Em Parquet gzip de 1 milhão de linhas e duas colunas gerado pelo pandas,
`ler_parquet` projetado + `agrupar_por` levou 950 ms no modo por linhas e
252 ms no modo colunar (7 execuções, mediana); o pico caiu de aproximadamente
725.600 KiB para 140.200 KiB. As saídas foram idênticas. O caminho colunar
agora alimenta os vetores tipados durante a leitura das páginas de campos
escalares, sem acumular colunas inteiras de `Value`.

Uma segunda otimização eliminou os `Value` temporários das páginas PLAIN
numéricas/booleanas e do vetor intermediário de páginas DICTIONARY escalares.
O benchmark reproduzível
`python3 scripts/benchmark_parquet_columnar.py /tmp/tilt-build/bin/tilt
--repetitions 7` mediu **97,0 ms no Tilt colunar, 1057,6 ms no Tilt por linhas
e 94,3 ms no pandas** para a mesma leitura Parquet + agregação. Os cinco
agregados foram comparados linha a linha, somando 250.344.875. O pico do
Tilt colunar nessa carga ficou em cerca de 35.716 KiB em medição separada.
Pandas já estava importado e executou no mesmo processo; cada amostra Tilt
incluiu a inicialização do processo. A diferença de ~3 ms entre pandas e
Tilt não sustenta uma conclusão geral sobre qual linguagem é mais rápida.
Com `--plain`, sem dicionário Parquet, uma rodada CPU-only separada de cinco
execuções mediu 118,4 ms no Tilt colunar, 858,8 ms por linhas e 87,4 ms no
pandas. Também nesse formato os agregados coincidiram.
Textos PLAIN, conversões lógicas especiais, listas e estruturas ainda usam
o decodificador geral.

Em medição separada com `/usr/bin/time`, o pico de memória foi ~653 MiB no
Tilt e ~181 MiB no pandas; com projeção, o Tilt caiu para ~513 MiB em uma
medição separada. Os números incluem custos de processo e
dependências. O modo por linhas do Tilt usa uma lista de mapas, enquanto o
pandas usa armazenamento colunar. A inferência mais forte é que a
leitura e a materialização de objetos dominam o caminho Tilt. Prioridades:
projeção/filtro antecipados na leitura, execução colunar ou delegação explícita
ao DuckDB, e redução do tamanho de `Value`. Medir cada etapa isoladamente antes
de substituir a representação da tabela.

Uma medição posterior separou o pipeline projetado em três programas, cada um
em processo novo (7 execuções, mediana): leitura CSV 632 ms, leitura mais
agregação 627 ms, pipeline inteiro 660 ms. A diferença entre os dois primeiros
está dentro da variação das execuções; a leitura e materialização de 1 milhão
de mapas dominam o tempo. A escrita dos cinco grupos acrescenta cerca de
30 ms. Esse resultado motivou o armazenamento colunar na entrada antes de
otimizar mais o agrupador.

## Listas, estruturas e vários processos

`python3 scripts/benchmark_parquet_nested.py /tmp/tilt-build/bin/tilt
--rows 100000 --repetitions 5` mediu leitura Parquet com listas numéricas,
estruturas opcionais e 20 row groups, em CPU-only. O modo por linhas levou
152,4 ms e atingiu 150.124 KiB; o modo colunar levou **42,1 ms** e atingiu
**12.848 KiB** (3,62× mais rápido, 11,7× menos RAM). Listas usam offsets e
elementos tipados; estruturas com formato estável guardam cada campo em uma
coluna. Formatos variáveis voltam ao armazenamento misto. Testes Parquet
conferem os valores aninhados nos dois modos, inclusive em vários row groups.

`env TILT_GPU=off TILT_BLAS=off python3
scripts/benchmark_distributed_columnar.py /tmp/tilt-build/bin/tilt
--repetitions 3` dividiu o mesmo milhão de linhas entre 1, 2 e 4 processos
locais. Cada processo leu seu próprio arquivo Parquet; geração e importação
ficaram fora da medição. Todos os agregados foram reconciliados com pandas.

| Processos | Linhas: leitura | Linhas: leitura + grupo | Colunar: leitura | Colunar: leitura + grupo |
|---:|---:|---:|---:|---:|
| 1 | 854,8 ms | 910,8 ms | 80,9 ms | **86,7 ms** |
| 2 | 489,2 ms | 512,6 ms | 48,5 ms | **61,0 ms** |
| 4 | 305,4 ms | 312,7 ms | 27,1 ms | **33,8 ms** |

O modo colunar escalou 2,57× de 1 para 4 processos sobre volume total fixo.
O merge final dos cinco grupos levou cerca de 0,02–0,03 ms. A diferença entre
as colunas de leitura e leitura+grupo sugere o custo incremental da agregação,
mas são programas separados e não um perfil interno. Isto mede um host com
cache de arquivos aquecido; não inclui rede, filesystem remoto nem barreiras
de treinamento.

No treino Parquet em cluster, quando há pelo menos um row group por rank, cada
rank agora processa apenas seus grupos durante as épocas e projeta somente os
atributos necessários. A passada inicial ainda lê os rótulos de todos os grupos,
mas só decodifica a coluna alvo. Testes de 2 e 4 ranks passaram; ainda falta
medir o tempo de checkpoint/barreira em máquinas separadas para quantificar a
escalabilidade do treinamento. Em host único, com diretório compartilhado local,
`scripts/benchmark_cluster_training.py` mediu duas épocas no mesmo arquivo
Parquet com 16 row groups (medianas de 3 execuções para 20 mil linhas e 2
execuções para 200 mil):

| Linhas | Ranks | Parede | Leitura inicial de rótulos | Checkpoint + barreiras |
|---:|---:|---:|---:|---:|
| 20 mil | 1 | 71,1 ms | 7,5 ms | 0 ms |
| 20 mil | 2 | 117,8 ms | 7,9 ms | 76,9 ms |
| 20 mil | 4 | 152,4 ms | 9,5 ms | 126,7 ms |
| 200 mil | 1 | 655,0 ms | 43,9 ms | 0 ms |
| 200 mil | 2 | 425,0 ms | 68,0 ms | 76,5 ms |
| 200 mil | 4 | 377,6 ms | 106,8 ms | 102,1 ms |

Os tempos de leitura e sincronização são o máximo observado entre ranks; o
restante do tempo de parede inclui inicialização, computação e espera. Com 20
mil linhas a sincronização supera o trabalho útil, mas com 200 mil linhas quatro
ranks reduzem a parede em 1,73×. Esta medição não prevê o custo de um filesystem
remoto; a média de parâmetros por checkpoint JSON ainda é um limite claro.

## Tensores na CPU

`scripts/benchmark_tensor_ops.py --iterations 12 --warmup 3` mede operações
repetidas com entradas FP32 prontas, incluindo a redução de checksum. Foram
fixadas 4 threads para MKL/OpenBLAS; NumPy usa sua biblioteca BLAS instalada.
A convolução NumPy é `sliding_window_view` + `einsum`, não uma chamada cuDNN
ou PyTorch, portanto representa somente essa implementação NumPy.

| Operação | Tilt portátil | Tilt com oneMKL | NumPy |
|---|---:|---:|---:|
| GEMM 256×256 | 1,382 | 0,239 | **0,210** |
| Conv2d NCHW 1×16×64×64, 32 filtros 3×3 | 8,546 | 0,987 | **0,342** |

CBLAS acelera o GEMM ~5,8× e a convolução ~8,7× em relação aos kernels
portáteis nesta máquina. GEMM fica próximo do NumPy; conv2d permanece ~2,9×
mais lenta. A CPU portátil continua funcional se BLAS não estiver instalada.
Para melhorar conv2d, perfilar a criação de im2col, alocações e escolha de
bloco; bibliotecas otimizadas ou pesos pré-transformados são opções posteriores.

## GPU com transferência de dados

`TILT_GPU=auto TILT_BLAS_LIBRARY=/home/tel/anaconda3/lib/libmkl_rt.so
MKL_NUM_THREADS=4 /tmp/tilt-build/tests/gpu_dispatch_benchmark 7` mede CPU e
CUDA no mesmo processo. Inclui cópias host→device→host, mas exclui a
inicialização CUDA e compilação NVRTC. A CPU usa oneMKL quando o tamanho atinge
o limiar do runtime; a GPU usa cuBLAS quando disponível e o kernel próprio
quando não. Os checksums CPU/GPU foram comparados com tolerância FP32.

| Operação | CPU | CUDA | Melhor |
|---|---:|---:|---|
| GEMM 128×128 | **0,044** | 0,189 | CPU 4,3× |
| GEMM 256×256 | **0,191** | 0,366 | CPU 1,9× |
| GEMM 512×512 | 0,986 | 0,980 | empate prático |
| GEMM 1024×1024 | 9,325 | **3,186** | CUDA 2,93× |
| Conv2d, entrada 16×16 | **0,109** | 0,165 | CPU 1,5× |
| Conv2d, entrada 64×64 | 0,968 | **0,401** | CUDA 2,41× |

O limiar de despacho deve considerar BLAS CPU e tamanho: transferências fazem
GPU perder nos casos pequenos. O runtime mantém `TILT_GPU=off` como padrão,
usa CPU automaticamente quando CUDA não está disponível e suporta
`TILT_GPU=auto` para aceleração seletiva. Ainda faltam tensores residentes no
device, fusão de operadores e backward na GPU; estes reduziriam as cópias
entre camadas, mas exigem desenho de vida útil e testes de treinamento.

## HTTP persistente

`python3 scripts/benchmark_http_client.py /tmp/tilt-build/bin/tilt --requests 40
--repetitions 5` usa um servidor HTTP/1.1 local com keep-alive e compara
40 chamadas JSON no mesmo processo Tilt. Medianas: libcurl nativa 13,9 ms;
`curl` CLI 203,3 ms. O ganho de 14,6× é para muitas chamadas pequenas na
mesma origem; não prevê ganhos equivalentes quando a latência da rede ou o
tempo do modelo LLM dominam. O benchmark desliga Nagle no servidor de teste
para não introduzir um atraso artificial de ACK em cada resposta curta.

## Complemento de 25/09/2026 — ponte Python e funções colunares

`scripts/benchmark_python_bridge.py` usa o mesmo processo `tilt rpc`, tabelas
de entrada prontas e função identidade. Cada amostra inclui serialização,
transporte e decodificação da resposta. São medianas de 5 repetições após um
aquecimento, na máquina descrita no início deste relatório:

| Linhas | JSON (lista de mapas) | Parquet temporário (Arrow) |
|---:|---:|---:|
| 1 mil | 3,1 ms | 3,3 ms |
| 10 mil | 32,0 ms | 9,3 ms |
| 100 mil | 385,9 ms | 63,7 ms |

Para 100 mil linhas, o transporte Parquet foi 6,1× mais rápido. O resultado
Python é `pyarrow.Table`, enquanto o JSON devolve lista de mapas; isso mede os
formatos úteis a cada caminho, não o custo de converter ambos para um mesmo
objeto Python. A ponte faz cópias e usa o filesystem local; não é zero-copy.

`scripts/benchmark_function_columnar.py` compara o mesmo CSV colunar, filtro
e agrupamento diretamente no pipeline ou dentro de uma `funcao`. Antes de
preservar a tabela colunar na chamada, as medianas de 5 repetições foram
207,5 ms (direto) e 698,3 ms (função). Depois, em outra rodada de 5 repetições,
foram 193,0 ms e 194,0 ms. As saídas coincidiram. As rodadas separadas não
permitem atribuir a diferença de 207 para 193 ms a uma otimização específica;
o ganho sustentado é a eliminação da penalidade grande ao chamar a função.
O filtro numérico tipado não mostrou ganho adicional claro no tempo total,
dominado pela leitura do CSV.

## Próximas intervenções, em ordem

1. Instrumentar leitura, agrupamento e escrita separadamente e oferecer
   projeção/predicados nas fontes Parquet/CSV, usando estatísticas quando
   disponíveis. A projeção já foi adicionada; faltam predicados e estatísticas.
   Meta: aproximar o pipeline de 181 ms e baixar o pico de RAM.
2. Expandir a representação colunar já implementada para mais operadores e
   reduzir os temporários `Value` restantes por página no leitor Parquet. Comparar também
   com DuckDB para consultas analíticas quando disponível.
3. Compactar `Value` e usar slots locais na VM; validar interpretador, VM e JIT
   com os mesmos programas antes de substituir a estrutura atual.
4. Perfilar conv2d com oneMKL e otimizar im2col/tiles; comparar com PyTorch
   CPU quando disponível, fixando threads e formas.
5. Introduzir residência de tensores na GPU, kernels fundidos e backward;
   refazer a curva de ponto de equilíbrio com e sem BLAS na CPU.

Estas medições não equivalem a uma classificação geral de linguagens: o
desempenho depende de bibliotecas, formas dos dados, disco, threads e overhead
de processo. O histórico em `bench/baseline.json` usa outra máquina/calibração;
seus alertas normalizados não são evidência de regressão nesta medição. O
comparador agora registra CPU/Python em baselines novos e não classifica um
baseline legado sem procedência como regressão; a comparação local foi
validada com um baseline criado em `/tmp`.
