# 16 — Desempenho: onde estamos e como melhorar

O Tilt é uma linguagem **declarativa para dados**. Para pipelines, o custo dominante
costuma estar nos conectores, no Parquet e nas operações de tensor em C++, e aí o
desempenho é razoável. Para **lógica** escrita na própria linguagem (laços,
recursão, transformações linha a linha), o interpretador é lento. Este capítulo
registra medições reais, explica onde o tempo vai e propõe um plano, em ordem de
retorno, com como medir cada passo.

## Medições de referência

Build Release, uma máquina Linux x86-64, uma execução cada (Python 3.14 como
referência). Reproduza com `bench/rodar.sh` — o que importa é comparar *antes/depois
na mesma máquina*, não os números absolutos.

| Caso | Interpretador | VM (`--vm`) | JIT (`--jit`) | Referência |
|---|---|---|---|---|
| Laço de 3 milhões de iterações (`bench/laco.tilt`) | 1,16 s | 1,18 s | **0,12 s** | CPython 0,36 s |
| `fib(30)`, 1,6 milhão de chamadas (`bench/fib.tilt`) | 1,50 s | 1,54 s | 2,03 s | CPython 0,18 s |
| CSV de 1 M linhas: `ler_csv` + `agrupar_por` + `escrever_parquet` (`bench/dados.tilt`) | 1,71 s | — | — | Python `csv` puro 2,85 s; DuckDB 0,17 s |

O que os números dizem:

- **Dados:** mais rápido que Python puro, mas ~10× mais lento que um motor colunar.
- **Lógica no interpretador/VM:** 3× (laço) a 8× (recursão) mais lenta que o CPython.
- **A VM não ganha do interpretador** (1,18 s contra 1,16 s): o bytecode ainda opera
  sobre o mesmo `Value` pesado e as mesmas tabelas de nomes.
- **O JIT só ajuda em laço de inteiros.** Em chamadas ele é até mais lento que a VM
  (2,03 s contra 1,54 s), porque cai para a VM a cada chamada. No Apple Silicon ele
  nunca ativa (só existe backend x86-64).

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

## Plano para a lógica (do maior ganho para o menor)

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

## Plano para os dados

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

- `bench/rodar.sh [caminho-do-tilt]` roda os três casos no interpretador, na VM e no
  JIT (gera `bench/vendas.csv` na primeira vez; o arquivo é ignorado pelo git).
- Compare *antes/depois na mesma máquina*, com o mesmo binário Release
  (`cmake --preset release`), e repita 3 vezes: os números variam com a carga.
- Próximo passo natural: um comando `tilt bench` e um job de CI que falha se um caso
  ficar mais de X% mais lento que a referência registrada, para que otimizações e
  novas *features* não escondam regressões.
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
