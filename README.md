# Tilt

Linguagem declarativa estilo YAML para **Machine Learning, Deep Learning,
Engenharia de Dados, LLMs e Agentes de IA**. Compilador e runtime em C++20,
sem LLVM e sem Rust.

Um mesmo arquivo `.tilt` descreve um pipeline de dados, treina um modelo, chama
um LLM, orquestra agentes e sobe um serviço HTTP — com a mesma sintaxe: 2
espaços de indentação, `chave: valor`, listas com `-`, sem `{ }`, `;` nem `()`
ruidosos.

```tilt
pipeline resumo_vendas:
  passos:
    - vendas = ler_csv "vendas.csv"
    - grandes = vendas.filtrar linha.valor >= 50
    - por_regiao = grandes.agrupar_por "regiao", { receita: somar "valor", n: contar }
    - para cada r em por_regiao:
        imprimir r.regiao, "->", r.receita, "(", r.n, "pedidos )"
```

- **Especificação da linguagem:** [`CLAUDE.md`](CLAUDE.md)
- **Guia de uso (13 capítulos):** [`docs/`](docs/README.md)
- **Repositório:** https://github.com/ailake-io/tilt

---

## Estado — `v0.1.0`

Toolchain interpretada **completa** (1ª passada de cada marco). O que roda hoje:

| Área | Funciona | Ainda é stub |
|---|---|---|
| Sintaxe | lexer, parser, semântica (`checar`), interpretador de árvore, literais `[...]`/`{...}` multilinha | assinaturas exóticas de `funcao` |
| Semântica | tipos, segredos, dispositivos, referências de ferramenta, resolução de nomes em `passos:`/`executar:` (`T030`), shape solver de `densa`/`linear` + formas literais `conv2d`/`norma_lote`/`matmul`/`reformar`/`transposta` (`T012`), inferência de tipos conservadora (`T011`: operadores, builtins, métodos, retorno) | shape solver das demais camadas, inferência de tipos além do subconjunto conservador |
| Dados | CSV, JSON, **Parquet nativo** (escrita PLAIN com **colunas opcionais/nulos**, compressão **gzip** (padrão) ou **snappy** (`codec:`), **DATA_PAGE_V2** (`paginas: "v2"`) e **listas**; leitura de **múltiplos row groups**, **dictionary encoding**, **gzip/deflate** via zlib em `dlopen`, **snappy**, páginas V2 e **listas** (REPEATED + anotação LIST, UTF8) — validado com pyarrow em `tests/parquet_test.sh` e com **Spark 3.5** em `tests/spark_test.sh`), **Delta Lake mínimo** (leitura/escrita, **append transacional** via `anexar_delta` — commit atômico por `rename`, single-writer — e **partições hive-style** (1 ou mais colunas) via `particionar_por:` — layout `<col>=<valor>/.../part-NNNNN.parquet` sem as colunas nos dados, `partitionValues` no log e colunas reidratadas na leitura com conversão de tipo, com **pruning** em `ler_delta "dir", onde: {col: val, ...}`; sem nulos/`/` em valor de partição; **evolução de schema no append** — coluna nova entra nullable no fim do `schemaString` com `metaData` novo no commit e linhas antigas leem `nulo` (union-by-name); remoção de coluna ou tipo divergente → erro claro — validados com pyarrow/delta-rs e com **Spark 3.5** (`spark.read.format("delta")` em `tests/spark_test.sh`) e **Iceberg** (`ler_iceberg`/`escrever_iceberg`/`anexar_iceberg` — catálogo Hadoop em diretório local, **Avro OCF próprio** para manifest list + manifest, Parquet nativo nos data files, append por cadeia de snapshots com parent — verificado com parser Avro independente em `tests/iceberg_test.sh`; **partições identity** (1 ou mais colunas) via `particionar_por:` — layout `col=valor/.../00000-0-<uuid>.parquet` sem as colunas nos dados, `partition-spec` no metadata, record `partition` no manifest e colunas reidratadas na leitura com conversão de tipo, com **pruning** em `ler_iceberg "dir", onde: {col: val, ...}`; sem nulos/`/` em valor de partição; os blocos **Avro OCF** dos manifests gravam com codec **deflate** (padrão), `null` ou **snappy** (`ICEBERG_AVRO_CODEC`) e a leitura aceita os três; **evolução de schema no append** — coluna nova entra `required: false` com field-id novo (`last-column-id` + 1), metadata versionado com `schema-id` novo e histórico de schemas estável, data files do append gravados com esses field-ids e linhas antigas leem `nulo` (union-by-name) — validado com **pyiceberg** e com **Spark 3.5** real (`spark.read` via `SparkCatalog` em `tests/spark_test.sh` — metadata `v<N>.metadata.json`, partição reidratada, pruning por bounds do manifest list)); **REST catalog opt-in** (fase 29: `ICEBERG_CATALOG=rest` + `ICEBERG_URI=http://host:porta` — `escrever_iceberg`/`anexar_iceberg`/`ler_iceberg` passam a operar via Iceberg REST Open API, subconjunto `loadTable`/`createTable`/`transactions` no namespace `default`; o tilt segue gravando os arquivos localmente na `<location>` `file://` e commita as locations no catalogo — mock + validação pyiceberg em `tests/iceberg_rest_test.sh`; Hadoop local continua o default sem as env vars), `fonte` **sqlite**/**postgres**/**duckdb**/**mysql** (SELECT via `dlopen` — DuckDB `libduckdb.so`, MySQL/MariaDB `libmariadb.so`/`libmysqlclient.so`), **ClickHouse** (HTTP nativo próprio) e **Elasticsearch/OpenSearch** (`fonte tipo: elasticsearch|opensearch` — `POST /<indice>/_search`; sem dependências), com gravação SQL genérica via **`executar_sql`** (`postgres://`/`sqlite://`/`duckdb://`/`mysql://`/`clickhouse://` — INSERT/UPDATE/DDL), Redis via **RESP nativo** (`ler_redis`/`escrever_redis` — **AUTH/SELECT**: userinfo `:senha@` e path `/db` na URL, ou opções `{senha:, banco:}` que vencem a URL — e genérico via **`redis_executar`** (comando RESP arbitrário) / **`redis_lote`** (pipeline numa conexão)), **Kafka** via **wire protocol 0.9 próprio** sobre TCP (`ler_kafka`/`escrever_kafka` — consumer groups com coordenação FindCoordinator/JoinGroup/SyncGroup e **commit automático de offset**, checkpoint natural para `fonte tipo: kafka` no `janela:`; sem dependências, broker por `KAFKA_BOOTSTRAP`), **MongoDB** via **wire protocol OP_MSG próprio** (`mongo_inserir`/`mongo_buscar`/`mongo_atualizar` (`$set`/`$inc`)/`mongo_deletar`/`mongo_criar_indice` — **CRUD básico completo** — `mongo_buscar` com projeção (`somente:`) e cursores com **getMore** (`lote:`); e `mongo_agregar` com pipeline de aggregation traduzido 1:1 para BSON — sem dependências, banco por `MONGO_URL`), cliente **HTTP genérico** (`http_get_json`/`http_post_json` — JSON sobre `curl`, com headers opcionais; ver guia 03), índice **Qdrant** (`armazenamento: "qdrant://..."`), **pgvector** (`armazenamento: "pgvector://colecao"` + `url:`), **Weaviate** (`armazenamento: "weaviate://host:porta/classe"` — REST via `curl`, GraphQL `nearVector`, auth por env `WEAVIATE_API_KEY`), **Pinecone** (`armazenamento: "pinecone://host/namespace"` — data plane via `curl`, sempre HTTPS, `PINECONE_API_KEY` obrigatória, índice pré-existente) e **Chroma** (`armazenamento: "chroma://host[:porta]/colecao"` — REST via `curl`, HTTP puro, sem auth, coleção get-or-create), **S3** (`ler_s3`/`escrever_s3`/`listar_s3`/`apagar_s3`/`copiar_s3`/`cabecalho_s3`/`s3_iniciar_upload`/`s3_enviar_parte`/`s3_concluir_upload`/`s3_abortar_upload` — SigV4 próprio incluindo query string e `x-amz-copy-source` assinados, GET/PUT/LIST/DELETE/HEAD/CopyObject/multipart, via env + `curl`), **TLS nos clientes TCP** (redis `rediss://` ou `{tls: verdadeiro}`, mongo `mongodb+srv://`, kafka `{tls: verdadeiro}` — OpenSSL em `dlopen` (`libssl.so.3`), trust store do sistema, `TILT_TLS_SKIP_VERIFY=1` p/ cert auto-assinado em testes; sem SASL/client-cert — ver `docs/guia-12-limitacoes.md`), `pipeline`, `verificar`, `ao_falhar`, `agenda` com **loop real**, streaming `janela:` (contagem com **janela deslizante** via `sobreposicao: M`, tempo e throttle; **offset persistente** em `<fonte>.tilt-offset` para fontes de arquivo — mapa por pipeline, `TILT_JANELA_ESTADO=memoria` desliga) | — |
| ML/DL | tensores f32 CPU (matmul multithread, `conv2d`, `norma_lote` como operações de tensor), `modelo` (inferência, `pesos:` de arquivo, `norma_camada`), `treino` (CE + quadrática, SGD/Adam, backward de `densa`/ativações/`norma_camada`), `carregador` | GPU só validado em `fake`; `conv2d`/`norma_lote` fora do `modelo` |
| LLM/RAG | `perguntar`, saída estruturada por `tipo`, `incorporar`, `indice` em memória (cosseno), no **Qdrant** (REST via `curl`), no **pgvector** (SQL sobre libpq, cosseno `<=>`), no **Weaviate** (REST via `curl`, GraphQL `nearVector`), no **Pinecone** (REST via `curl`, sempre HTTPS, data plane) e no **Chroma** (REST via `curl`, HTTP puro, coleção get-or-create) | rede real precisa de `curl` |
| Agentes | `ferramenta`, `agente.responder` (planner iterativo), `equipe` (sequencial/paralelo/supervisor) | planner tolerante a protocolo (1 linha/turno) |
| HTTP | `servico`/`rota`, validação de `entrada:`, middleware **`meio:`** (aborta via `responder:`, escopo compartilhado com a rota), `tilt servir` (epoll + keep-alive + arena por requisição + **pool de rotas paralelo** `--threads N`, Linux; respostas pipelined em ordem por conexão) | — |
| Execução | VM de bytecode p/ `funcao` pura **e pipelines** (`tilt executar --vm`), codegen nativo **x86-64 e ARM64** p/ **programa inteiro** (texto, decimal, listas, `para cada`, pipelines; saída idêntica ao interpretador) | builtins fora do subconjunto nativo |
| Tooling | `checar --json`, `referencia`, `tilt lsp` (completion, **hover**, **go-to-definition**, **signatureHelp**, **formatting**) + `completar`, `checar_tilt`, extensão VS Code (realce + LSP, vsix empacotável com `npm run package`), **stdlib** (`io`/`nn`/`rede`) com `importar` resolvendo módulos | publicação no Marketplace |

Detalhes em [`docs/guia-12-limitacoes.md`](docs/guia-12-limitacoes.md).

---

## Instalação

### Do código-fonte

```bash
git clone https://github.com/ailake-io/tilt
cd tilt
sh scripts/install.sh                 # instala em ~/.local
# ou:  sh scripts/install.sh --prefix=/usr/local     (talvez com sudo)
```

Requisitos: `cmake >= 3.20`, compilador C++20 (GCC 11+ / Clang 14+). Instala
`tilt` em `<prefix>/bin` e os exemplos em `<prefix>/share/tilt`. Remover:
`sh scripts/install.sh --uninstall`.

### Binários pré-compilados

Cada tag `v*` publica `tilt-<versao>-<os>-<arch>.tar.gz` (+ `.sha256`) em
[Releases](https://github.com/ailake-io/tilt/releases): Linux x86_64 (estático,
sem dependência de libstdc++) e macOS arm64. Extraia e ponha `bin/tilt` no
`PATH`. `curl` é necessário em runtime apenas para chamadas HTTP externas
reais (LLM, S3, bancos vetoriais, Elasticsearch/OpenSearch, HTTP genérico).

### Pacote local

```bash
cmake --preset release && cmake --build --preset release
cd build/release && cpack           # gera .tar.gz + .deb + .rpm de uma vez
```

Instalação, requisitos, tarball, `.deb`, checksum e Release automatizada por
tag: [docs/guia-13-instalacao.md](docs/guia-13-instalacao.md).

---

## Primeiro programa

```tilt
# ola.tilt
funcao faixa n -> texto:
  se n >= 100:
    retornar "grande"
  retornar "pequena"

pipeline ola:
  passos:
    - total = 40 + 62
    - imprimir "total:", total, "->", faixa(total)
    - para cada k em intervalo 3:
        imprimir "  item", k
```

```bash
tilt checar ola.tilt      # valida sintaxe, indentação e tipos
tilt executar ola.tilt    # roda
```

```
== pipeline ola ==
total: 102 -> grande
  item 0
  item 1
  item 2
```

---

## Comandos

| Comando | Descrição |
|---|---|
| `tilt checar <a> [--json]` | valida sintaxe, indentação, tipos, segredos, dispositivos, referências |
| `tilt executar <a> [--agendar]` | roda no interpretador (VM para `funcao` pura) |
| `tilt executar --vm <a>` | roda pipelines pelo bytecode VM (fall-back por pipeline) |
| `tilt servir <a> [--porta N] [--requisicoes N] [--threads N]` | sobe o `servico` HTTP declarado |
| `tilt compilar <a> --saida <bin> [--asm] [--arch x86_64\|arm64]` | binário nativo x86-64/ARM64 (programa inteiro no subconjunto da VM) |
| `tilt completar <a> --linha L --coluna C [--json]` | candidatos de autocomplete |
| `tilt lsp` | servidor Language Server (stdio) |
| `tilt referencia` | referência compacta da linguagem |
| `tilt ast <a>` / `tilt tokens <a>` | despeja AST / tokens (debug) |
| `tilt versao` / `tilt ajuda` | versão / ajuda |

Detalhe de cada flag e código de saída em [`docs/guia-08-cli.md`](docs/guia-08-cli.md).

### Variáveis de ambiente

| Var | Efeito |
|---|---|
| `TILT_LLM` | `mock` (respostas/embeddings determinísticos, offline) · vazio → `curl` real |
| `TILT_GPU` | `off` (padrão) · `auto` (CUDA se `dispositivo:` pedir) · `fake` (dispatch de GPU com math de CPU) |
| `TILT_VM_DEBUG` | `1` despeja o bytecode das `funcao`s compiladas |
| `TILT_STDLIB_PATH` | diretórios com módulos `importar` (sep. `:`), consultados antes de `../share/tilt/stdlib` |
| `TILT_JANELA_ESTADO` | `memoria` desliga o offset persistente do streaming `janela:` |
| `TILT_TLS_SKIP_VERIFY` | `1` desliga verificação de certificado TLS nos clientes (testes com cert auto-assinado) |
| `CC` | compilador C usado por `tilt compilar` (padrão `cc`) |
| `NO_COLOR` | desliga cor nos diagnósticos |

---

## Exemplos

`exemplos/` (instalados em `<prefix>/share/tilt/exemplos/`):

| Arquivo | Mostra | Como rodar |
|---|---|---|
| `soma.tilt` | cômputo, `funcao`, laço | `tilt executar` |
| `resumo_vendas.tilt` | CSV → filtrar/agrupar/agregar | `tilt executar` |
| `qualidade.tilt` | `verificar` (qualidade de dados), `ordenar_por` | `tilt executar` |
| `fontes.tilt` | `fonte` JSON, `escrever_json` | `tilt executar` |
| `inferencia.tilt` | tensores, `modelo`, forward CPU | `tilt executar` |
| `treino.tilt` | `treino` (backprop + Adam), `carregador` | `tilt executar` |
| `rag_llm.tilt` | `indice` (RAG) + `perguntar` | `TILT_LLM=mock tilt executar` |
| `agente.tilt` | `ferramenta` + `agente.responder` | `TILT_LLM=mock tilt executar` |
| `copiloto.tilt` | agente que revisa Tilt via `checar_tilt` | `TILT_LLM=mock tilt executar` |
| `servico.tilt` | `servico` / `rota` HTTP | `tilt servir` |
| `nativo.tilt` | subconjunto compilável (fatorial, ackermann) | `tilt compilar` |

---

## Documentação

| # | Guia |
|---|---|
| 01 | [Sintaxe](docs/guia-01-sintaxe.md) — indentação, literais, variáveis, `funcao`, controle de fluxo, operadores |
| 02 | [Tipos](docs/guia-02-tipos.md) — `tipo`, escalares, `lista`/`mapa`/`opcional`, `tensor`, `tabela`, `fluxo`, união-literal |
| 03 | [Engenharia de dados](docs/guia-03-dados.md) — `fonte`, `pipeline`, métodos de tabela, `verificar`, `ao_falhar`, `agenda` |
| 04 | [ML e Deep Learning](docs/guia-04-ml-dl.md) — tensores, `modelo`, `treino`, `carregador`, `dispositivo`/GPU |
| 05 | [LLMs e RAG](docs/guia-05-llm-rag.md) — `llm`, `perguntar`, saída estruturada, `incorporar`, `indice` |
| 06 | [Agentes](docs/guia-06-agentes.md) — `ferramenta`, `agente`, `equipe`, `memoria` |
| 07 | [Serviços HTTP](docs/guia-07-http.md) — `servico`, `rota`, `entrada`, `responder`, `tilt servir` |
| 08 | [CLI](docs/guia-08-cli.md) — cada comando, flags, códigos de saída, variáveis de ambiente |
| 09 | [VM e nativo](docs/guia-09-vm-nativo.md) — subconjunto da VM de bytecode e do codegen |
| 10 | [IA e editores](docs/guia-10-ia-editores.md) — `checar --json`, `referencia`, `checar_tilt`, LSP, Neovim |
| 11 | [Diagnósticos](docs/guia-11-diagnosticos.md) — todos os códigos `Tnnn` |
| 12 | [Limitações](docs/guia-12-limitacoes.md) — status da 1ª passada |
| 13 | [Instalação e distribuição](docs/guia-13-instalacao.md) — do código-fonte, binários pré-compilados, pacotes e Release |

---

## Arquitetura

```text
Fonte .tilt
   │
   ▼  src/lexer/       -> tokens (motor de indentação INDENT/DEDENT)
   ▼  src/parser/      -> AST (descida recursiva, std::unique_ptr)
   ▼  src/semantic/    -> símbolos, tipos, formas de tensor  (tilt checar)
   ▼  src/interp/      -> interpretador de árvore            (tilt executar)
   │     └─ src/vm/       funcao/pipeline -> bytecode -> VM de pilha
   │     └─ src/codegen/  subconjunto da VM -> Assembly x86-64/ARM64  (tilt compilar)
   └─ src/runtime/     value, tensor, json, parquet, delta, iceberg, conectores, llm, vectorstore, http, gpu_runtime
      src/lsp/          completion + servidor LSP              (tilt lsp)
```

Layout do repositório em [`CLAUDE.md` §4](CLAUDE.md).

---

## Desenvolvimento

```bash
cmake --preset debug     # AddressSanitizer + UBSan + -Werror
cmake --build --preset debug
ctest --preset debug --output-on-failure     # golden + http + par + native + lsp
```

Testes de ouro em `tests/golden/` (`entrada.tilt` → `esperado.*`); regenerar:
`UPDATE=1 sh tests/run_golden.sh ./build/release/bin/tilt tests/golden`.
CI: `.github/workflows/ci.yml` (Linux gcc + macOS + sanitizers) e
`release.yml` (tag `v*` → tarballs no GitHub Release).

---

## Roadmap

`M0`–`M12` + instalador + GPU + tooling de IA + conectores (S3, Kafka,
MongoDB, Iceberg, DuckDB, MySQL, ClickHouse, Elasticsearch/OpenSearch,
Redis, SQLite/Postgres) + bancos vetoriais (Qdrant, pgvector, Weaviate,
Pinecone, Chroma) + streaming `janela:` + append Delta/partição composta e
pruning (Delta/Iceberg) + Parquet snappy/V2/listas + codecs Avro + HTTP
genérico + stdlib (`io`/`nn`/`rede`) + LSP (hover/definition/signatureHelp/
formatting) + codegen ARM64 concluídos (1ª passada). Publicação da extensão
no Marketplace é automática em tags `v*` via `release.yml` ao configurar o
secret `VSCE_PAT` no repositório.

## Licença

MIT — ver [`LICENSE`](LICENSE).
