# 16 — Desempenho: onde estamos e como melhorar

O Tilt é uma linguagem **declarativa para dados**. Para pipelines, o custo dominante
costuma estar nos conectores, no Parquet e nas operações de tensor em C++, e aí o
desempenho é razoável. Para **lógica** escrita na própria linguagem (laços,
recursão, transformações linha a linha), o interpretador é lento. Este capítulo
registra medições reais, explica onde o tempo vai e propõe um plano, em ordem de
retorno, com como medir cada passo.

## Medições de referência

Build Release, uma máquina Linux x86-64 (8 núcleos), melhor de 3 execuções, Python 3.14
como referência. Reproduza com `bench/rodar.sh` (tabela legível) ou
`bench/comparar.py` (normaliza pela velocidade da máquina e falha em regressão). O que
importa é comparar *antes/depois na mesma máquina*, não os números absolutos.

| Caso | Antes | Agora | Referência |
|---|---|---|---|
| Laço de 3 milhões de iterações, interpretador | 1,16 s | **0,56 s** | CPython 0,36 s |
| Laço de 3 milhões, VM (`--vm`) | 1,18 s | **0,55 s** | |
| Laço de 3 milhões, JIT (`--jit`) | 0,12 s | 0,12 s | |
| `fib(30)`, 1,6 milhão de chamadas, interpretador/VM | 1,50 / 1,54 s | **0,41 s** | CPython 0,18 s |
| `fib(30)`, JIT | 2,03 s | 1,60 s | (cai para a VM a cada chamada) |
| CSV 1 M linhas: `ler_csv` + `agrupar_por` + `escrever_parquet` | 1,71 s | **0,73 s** | Python `csv` 2,85 s |
| `ler_csv` de 1 M linhas | 1,21 s | **0,53 s** | |
| `ordenar_por` em 1 M linhas (com a leitura) | 4,6 s | **1,0 s** | |
| `ler_parquet` de 1 M linhas | 2,5 s | **1,7 s** | |
| `escrever_parquet` de 1 M linhas | ~1,0 s | **~0,55 s** | |
| `sql` sobre o CSV com DuckDB (agregação de 1 M linhas) | — | **0,19 s** | DuckDB CLI 0,17 s |

## O que já foi feito

Ordem cronológica; cada item é um commit com o antes/depois na mensagem.

**Lógica (VM e interpretador)**
- Operadores pré-decodificados na VM (`BinOp`), aritmética inteira/decimal *in place* sem
  alocar `Value` temporário, escalares copiados campo a campo.
- Chamada de função VM → VM direta: quadro novo na mesma pilha, sem `Env`, sem mutex, sem
  vetor de argumentos por chamada; tabela de destinos por chunk (com cache do último).

**Dados**
- `ler_csv`: arquivo inteiro num buffer, varredura sem alocar por célula, cabeçalho
  resolvido uma vez e, acima de ~2 MB, leitura em até 8 threads (faixas alinhadas a `\n`).
- `agrupar_por`, `filtrar`, `derivar`: sem cópias de `Value` por linha, um `Env` reaproveitado.
- `ordenar_por`: ordena índices sobre chaves extraídas uma vez (antes copiava a tabela e
  procurava a coluna a cada comparação); estável.
- Parquet: dicionário por `unordered_map` (teto de 1024 distintos), gzip no nível rápido,
  colunas geradas em paralelo na escrita (sem criptografia) e valores *movidos* na leitura.
- **`sql`** (e `tabela.sql`) roda SQL sobre tabelas em memória; com DuckDB lê CSV/Parquet
  direto, 6× mais rápido que `ler_csv` + `agrupar_por` (guia 03).

**Ferramentas**
- `bench/comparar.py` + `bench/baseline.json`: sete casos normalizados por uma calibração
  da máquina; falha se algum ficar mais de 1,6× pior que a referência.

## Próximos passos

Em ordem de prioridade. O critério é o que mais pesa em **limpeza de dados e pipelines**;
lógica pura em laços fica por último porque o gargalo dela (o `Value` de 120 bytes) é uma
mudança grande e isolada.

1. **API de limpeza de dados nativa** (o que falta para não precisar cair em `derivar`
   linha a linha ou em SQL): `remover_nulos`, `preencher_nulos`, `renomear`,
   `remover_colunas`, `converter` (tipos), `deduplicar`, `juntar` (join), `empilhar`,
   `descrever` (perfilagem: nulos, distintos, min/max/média por coluna), `amostra`,
   `contar_valores` e conversão de datas. Detalhes em [guia 14](guia-14-roteiro.md).
2. **`ler_parquet` (1,7 s)**: falta o custo de criar cada `Value`; leitura por row group em
   paralelo e materialização direta nos mapas.
3. **`derivar` (~1 s por 1 M de linhas, 1,5 GB)**: avaliar expressões simples (coluna
   operador constante/coluna) sem passar pelo interpretador, e paralelizar por faixas.
4. **VM**: superinstruções (`local op local`, `local op const`, comparar-e-saltar) montadas
   em tempo de carga sem mexer no bytecode que o JIT e o cache `.tiltc` leem; ganho
   estimado de 25–30% em laços.
5. **DuckDB como motor opcional de `agrupar_por`/`juntar`** em tabelas grandes, com o mesmo
   resultado (ordem e tipos) do caminho nativo.
6. **`Value` compacto (~16–24 bytes)**: tag + união escalar e um ponteiro para a carga
   pesada (texto, lista, mapa, tensor, função). É a mudança que destrava lógica, memória
   (hoje ~650 bytes por linha de 3 colunas) e leitura de Parquet. Envolve ~1 800 pontos de
   uso (`.map`, `.list`, `.s`...): fazer em branch próprio, com a suíte completa e os
   *goldens* a cada etapa.
7. **JIT com chamadas e decimais** e backend ARM64, depois do item 6.
8. **CI**: rodar `bench/comparar.py` num job Linux (não bloqueante no início, para calibrar
   a tolerância) e, depois, tornar bloqueante.

## Por que a lógica é lenta

1. **`Value` é gordo.** Cada valor carrega `kind`, `bool`, `int64`, `double`, um
   `std::string` e três `shared_ptr` (lista, mapa, tensor): dezenas de bytes e
   construtores/destrutores caros até para um inteiro. Copiar um `Value` no laço
   quente custa mais que a própria soma.
2. **Variáveis por nome.** `Env` guarda `unordered_map<string, Value>`; cada leitura
   ou escrita é um hash de texto, e cada iteração de `enquanto`/`para cada` cria um
   `Env` novo (alocação).
3. **Chamada de função cara.** `call_function` toma um mutex (`vm_chunks_mutex_`)
   para achar o bytecode em um mapa e monta um `Env` novo por chamada.
4. **A VM não usa os tipos.** O checker já infere tipos, mas a VM emite as mesmas
   operações genéricas e checa a tag de cada operando em toda instrução.
5. **Tabela = lista de mapas.** Uma `tabela` de 1 M linhas são 1 M `ValueMap` com
   chaves em texto e valores boxeados: muita memória, pouca localidade de cache.

## Plano de fundo: lógica (detalhe do item 6 acima)

1. **`Value` compacto (~16 bytes).** Tag + union (`int64`/`double`/`bool`) e um único
   ponteiro para a carga pesada (texto, lista, mapa, tensor, função). Inteiros e
   decimais deixam de alocar e de copiar strings. Esperado: 2–3× em laço e chamada.
   Medir: `bench/laco.tilt` e `bench/fib.tilt`.
2. **Variáveis por slot.** Uma passada depois do checker atribui a cada variável um
   índice de quadro; `Env` vira um vetor. Fim do hash por acesso e da alocação por
   iteração. Esperado: 1,5–2× adicional.
3. **Chamadas baratas.** Guardar o chunk compilado na própria declaração da função
   (sem mapa nem mutex no caminho quente), reaproveitar o quadro e evitar copiar
   argumentos. Alvo: aproximar `fib(30)` do CPython (hoje 8× atrás).
4. **VM de registradores com tipos.** Usar os tipos do checker para emitir
   `soma_int`, `menor_int`, etc. sem checar a tag, e manter inteiros em registradores.
   Só então a VM passa a valer a pena em relação ao interpretador de árvore.
5. **JIT com chamadas e decimais**, e **backend ARM64** (`macos-latest` e servidores
   Graviton hoje não têm JIT). Depende dos passos 1–4 para o custo compensar.

## Plano de fundo: dados (parte já feita acima)

1. **Tabela colunar.** Guardar cada coluna como vetor tipado (`int64`, `double`,
   texto por dicionário), como no Arrow, em vez de lista de mapas. `somar`, `contar`,
   `filtrar` e `agrupar_por` passam a varrer vetores contíguos (SIMD). É o maior
   ganho para dados (5–10×) e reduz a memória em ordem de grandeza. Medir:
   `bench/dados.tilt` e o uso de memória (`/usr/bin/time -v`).
2. **Paralelismo.** Ler o CSV por blocos em várias threads (o arquivo é dividido em
   faixas de bytes alinhadas a `\n`) e agregar por thread, com um *merge* no fim.
   Nas fontes com row groups (Parquet) paralelizar por grupo.
3. **Delegar ao DuckDB.** O runtime já carrega a `libduckdb` por `dlopen`; agregações
   e junções grandes podem ser traduzidas para SQL e executadas lá, mantendo a mesma
   sintaxe do Tilt. O alvo é chegar perto dos 0,17 s da referência.
4. **Planos preguiçosos com pushdown.** Já existe para fontes SQL (`pushdown:`);
   estender a colunas e filtros de CSV, Parquet e Delta, pulando row groups pelas
   estatísticas e lendo só as colunas usadas.
5. **Rede.** Trocar o subprocesso `curl` (um processo por chamada) por um cliente
   HTTP nativo sobre a camada TLS que já existe, com *keep-alive* — latência menor
   em S3, LLM e Elasticsearch. E executar passos independentes de um `pipeline` (ou
   as iterações de um `para cada` sem dependência) em paralelo.

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
  antes de mesclar.

## O que não é gargalo

Treino e inferência de tensores (matmul/conv em C++ com AVX e *thread pool*), leitura
e escrita de Parquet/Delta/Iceberg e os conectores de rede já são código nativo; a
otimização de lógica acima não os afeta. Para modelos grandes o ganho vem da GPU
(CUDA ainda não validada em hardware real — ver [guia 12](guia-12-limitacoes.md)).
