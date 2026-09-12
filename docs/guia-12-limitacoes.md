# 12 — Limitações (1ª passada)

Cada marco `M0`–`M12` foi entregue em "1ª passada": o caminho principal
funciona, mas há bordas conhecidas. Lista do que **ainda não** funciona.

## Sintaxe / parser

- `e` / `ou` / `nao` / `contem` são reservadas — não servem como nome de
  variável, parâmetro ou loop var.
- Assinatura de `funcao` com parâmetros compostos é reconhecida de forma
  simples; casos exóticos podem se perder.

## Semântica

- `tilt checar` resolve nomes dentro de `passos:` / `executar:` (`T030`):
  escopo global mais variáveis implícitas (`linha`, `entrada`, `epoca`,
  `metricas`, `passo`, `resultado`) e campos de `entrada:`. Nomes fora
  disso são reportados.
- O solver de formas (`T012`) cobre a cadeia `densa`/`linear` nos `modelo`s
  (propaga a última dimensão a partir da anotação `entrada: tensor[...]` e
  rejeita `linear: [a, b]` com `a` incompatível) e, nos corpos de
  `funcao`/`pipeline`/`servico`, operações de tensor com formas literais ou
  anotadas: `conv2d` (rank 4, canais, núcleo vs. entrada, `passo:`),
  `norma_lote` (rank >= 2), `softmax`/ativações/`norma_camada` (preservadas),
  `reformar` (n. de elementos), `transposta` (2D) e `matmul` 2D. Fora do
  solver: formas através de chamadas de `funcao` ou condicionais, dimensões
  `_`/não literais, broadcast parcial e `conv2d` com formas dinâmicas —
  nesses casos a validação de dimensão continua acontecendo em runtime.
- Não há inferência completa de tipos: o que `checar` cobre hoje (`T011`) é
  o subconjunto evidente — operadores aritméticos/comparação com ambos os
  lados de tipo conhecido (rejeita `lista + 1`, `"a" - 1`, `"a" < 1`, mas
  aceita `texto + numero` e `"a" < "b"`, que o runtime suporta), builtins com
  aridade e 1º/2º argumento tipados (ex.: `tamanho 42`, `ler_csv 123`),
  métodos/campos de receiver conhecido (ex.: `"abc".matmul`, `t.filtrar` em
  tensor, `5.maiusculas`) e retorno de `funcao` anotada (`-> texto` com
  `retornar 42` — `inteiro` amplia para `decimal`, união de literais aceita
  texto). Fora daí o tipo vira "desconhecido" e segue sem verificação:
  tipos através de chamadas de `funcao`, campos dinâmicos de mapas/tabelas,
  `verificar`/`ao_falhar`, agregações em colunas e broadcast parcial.

## Dados

- Parquet é nativo (reader/writer próprio, zero dependências de link): a
  escrita é PLAIN com compressão **gzip** (padrão) ou **snappy** (`codec:
  "snappy"` — compressor literal-only, sem ganho de espaço mas interoperável),
  páginas DATA_PAGE **v1** (padrão) ou **v2** (`paginas: "v2"`), um row group
  por arquivo, com colunas REQUIRED ou OPTIONAL (nulos via definition levels
  RLE) e **listas de escalares** (anotação LIST, elementos sempre required na
  escrita). A leitura cobre múltiplos row groups, campos REQUIRED/OPTIONAL/
  REPEATED (listas aninhadas de escalares, inclusive o element OPTIONAL que o
  pyarrow grava — erro claro apenas para elemento nulo de fato), páginas v1 e
  v2, PLAIN e DICTIONARY (PLAIN_DICTIONARY/RLE_DICTIONARY) e os codecs
  gzip/deflate (zlib via `dlopen`) e **snappy** (codec próprio, sem dlopen).
  Ainda fora do subconjunto: dictionary encoding na escrita, listas de
  listas/structs, elementos nulos em listas e tipos físicos fora de
  BOOLEAN/INT64/DOUBLE/BYTE_ARRAY.
- Delta Lake é mínimo: `escrever_delta` sobrescreve a tabela (recria a versão
  0); o append existe via `anexar_delta` (nova versão por commit atômico de
  `rename`, validação de schema por nome com evolução limitada — ver abaixo —,
  single-writer — sem locks/optimistic concurrency). Partições hive-style
  suportam **uma ou mais colunas** (`particionar_por: "col"` ou
  `particionar_por: ["c1", "c2"]`, layout `<c1>=<v1>/<c2>=<valor>/part-NNNNN.parquet`,
  colunas reidratadas na leitura) e há **pruning** de partições em
  `ler_delta ... onde: {...}` (igualdade; predicados em coluna de partição pulam
  arquivos inteiros pelo log, o resto filtra linhas). Ainda assim: valor nulo
  em coluna de partição e valores com `/` não são suportados (erro claro, sem
  `__HIVE_DEFAULT_PARTITION__` nem escaping) e não há checkpoints; a leitura
  herda o subconjunto do Parquet acima. **Evolução de schema (fase 27)**: o
  append aceita colunas a mais — toda coluna antiga presente (ordem livre),
  coluna nova entra nullable no fim do `schemaString` com `metaData` novo no
  commit; arquivos antigos ficam sem a coluna e a leitura projeta nulo
  (union-by-name). Remover coluna ou mudar o tipo de uma existente → erro
  claro.
- Iceberg é de 1ª passada: o catálogo default é **Hadoop** (diretório local).
  Há um **REST catalog opt-in** (fase 29: `ICEBERG_CATALOG=rest` +
  `ICEBERG_URI`) falando o subconjunto `loadTable`/`createTable`/`transactions`
  do Iceberg REST Open API no namespace `default` — sem paginação, sem OAuth,
  location `file://` apenas (o tilt grava os arquivos localmente e commita as
  locations) e single-writer como no Hadoop; sobrescrita de tabela existente
  mantém o partition spec (divergência → erro claro). Sem as env vars o modo
  Hadoop continua, byte a byte. Demais limites: a
  leitura cobre o mesmo subconjunto do Parquet acima (tabelas de outros
  escritores sem garantia além dele) e single-writer (sem locks nem optimistic
  concurrency);
  `escrever_iceberg` sobrescreve a tabela (recria a versão 0) e o append é via
  `anexar_iceberg` (novo snapshot por commit atômico de `rename`; o manifest
  do novo snapshot lista os arquivos ativos como EXISTING + o ADD — além da
  cadeia de pais com adds menos removes).
  **Evolução de schema (fase 27)**: o append aceita colunas a mais — toda
  coluna antiga presente (ordem livre), coluna nova entra optional no fim do
  schema com **field-id novo** (`last-column-id` + 1), metadata versionado com
  `schema-id` novo mantendo o histórico (ids antigos estáveis) e os data files
  do append gravados com esses field-ids; leitura projeta nulo nas linhas dos
  arquivos antigos (union-by-name por field-id/nome — validado com pyiceberg).
  Remover coluna ou mudar o tipo de uma existente → erro claro.
  Partições suportam **uma ou mais colunas** (composta: `particionar_por:
  ["c1", "c2"]`, field-ids 1000, 1001, ...) e só com transform `identity`
  (layout `<c1>=<v1>/<c2>=<valor>/00000-0-<uuid>.parquet` sem as colunas no
  parquet, record `partition` no manifest e colunas reidratadas na leitura com
  conversão de tipo), com **pruning** em `ler_iceberg ... onde: {...}`
  (igualdade; predicados em coluna de partição pulam data files inteiros pelos
  manifests, o resto filtra linhas). Mas: valor nulo em coluna de partição,
  valores com `/` e coluna repetida não são suportados (erro claro, sem
  escaping), não há partitions summary nos manifests e data sequence numbers
  são sempre 0. A estrutura escrita (metadata, manifest list, manifest e
  parquet com field-ids) carrega no **pyiceberg**.
- Todos os conectores planejados rodam — a lista de stubs de conectores está
  vazia. CSV, JSON, Parquet, Delta, Iceberg, SQLite, Postgres, DuckDB, MySQL/
  MariaDB, ClickHouse, Elasticsearch/OpenSearch, Redis, Kafka, MongoDB, Qdrant,
  pgvector, Weaviate, Pinecone, Chroma, S3 e Spark (via Livy) rodam.
- Spark via Livy (`fonte tipo: spark`/`spark_sql`/`spark_executar`): REST/JSON
  pelo cliente HTTP genérico (subprocesso `curl`) — sessões **não são
  fechadas** pelo cliente (reuso deliberado: ele lista `GET /sessions` e pega
  a primeira idle com o `kind` da `lingua:`; o `conf:` só vale na criação e
  não casa sessão por conteúdo de conf); polling de statement com timeout
  fixo de ~120s (sem parametrizar intervalo/orçamento); `spark_sql` sempre
  materializa o resultado inteiro em memória via `toJSON` (sem streaming) e o
  parse assume array de objetos JSON; `spark_executar` devolve só o
  `data.text/plain` do último resultado (sem imagens/HTML); sem auth
  (Kerberos/Bearer), sem HTTPS próprio e sem gerenciamento de fila de
  statements — Livy sem auth em rede interna é o alvo.
- Elasticsearch/OpenSearch (`es_buscar`/`es_executar`/`fonte tipo:
  elasticsearch|opensearch`): REST/JSON puro pelo cliente HTTP genérico
  (subprocesso `curl`). `es_buscar` cobre só `_search` (DSL em texto ou mapa)
  e devolve `{total, hits}` com os hits achatados um nível (`_id` + campos de
  `_source`); agregações vêm cruas em `agregacoes` sem conveniências extras.
  Qualquer outro endpoint (indexação, `_delete_by_query`, `_cat`, settings)
  é via `es_executar`, que devolve o JSON parseado ou `texto` cru quando a
  resposta não é JSON. Auth só Basic (userinfo da URL ou env
  `ELASTIC_USER`/`ELASTIC_PASSWORD`), sem API keys/SASL/SSO, e HTTP apenas —
  esquema `https://` ainda não é configurável na URL (use o `es_executar`
  com reverse proxy local ou a rede interna).
- MongoDB (`mongo_inserir`/`mongo_buscar`/`mongo_atualizar`/`mongo_deletar`/
  `mongo_criar_indice`/`mongo_agregar`): BSON + OP_MSG próprios com CRUD
  básico completo — restam: `mongo_agregar` lê só o `firstBatch` do cursor
  (sem `getMore`; use `$limit`/`$skip` para caber no primeiro batch) e não
  valida as etapas (erro de pipeline vira erro claro do servidor), update só
  com `$set`/`$inc` (sem `$unset` e demais operadores), projeção de
  `mongo_buscar` só whitelist (`somente:`; sem exclusões tipo `{campo: 0}`),
  sem índices de texto/TTL, filtro de `mongo_buscar` só por igualdade exata
  top-level (combinado por E), `mongo_deletar` remove sempre todos que casam
  (sem `limit 1`), TLS via esquema `mongodb+srv://` (sem lookup DNS SRV), sem
  `OP_COMPRESSED`; document sequences (section kind 1) são puladas na leitura;
  uma conexão (com handshake `isMaster`) por chamada e payload inteiro em
  memória; banco por `MONGO_URL` (path) ou opção `banco:`.
- Kafka (`ler_kafka`/`escrever_kafka`/`fonte tipo: kafka`): wire protocol
  0.9-era — consumer groups com rebalanceamento `"roundrobin"` real (o líder
  calcula o assignment e o SyncGroup o distribui; heartbeat a cada 3s em
  thread; rejoin com retomada do offset commitado em
  RebalanceInProgress/IllegalGeneration), porém a detecção de entrada/saída de
  membros só é revalidada no próximo heartbeat ou na próxima chamada — sem
  revogação imediata de um fetch em andamento, sem SASL (TLS via
  `{tls: verdadeiro}`), produce v1/fetch v1 apenas, um broker líder por
  chamada e payload inteiro em memória.
- S3 (`ler_s3`/`escrever_s3`/`listar_s3`/`apagar_s3`/`copiar_s3`/
  `cabecalho_s3`/`s3_iniciar_upload`/`s3_enviar_parte`/`s3_concluir_upload`/
  `s3_abortar_upload`): REST com query string assinada (ListObjectsV2,
  multipart) — sem presigned URLs, sem versionamento (`versionId`) e payload
  inteiro em memória (multipart incluído, sem streaming de parte em disco);
  o parse dos XMLs de resposta (listagem, multipart) é por string simples
  (conteúdo de `<Key>`/`<UploadId>`/`<ETag>`, entidades básicas); HTTP depende
  do binário `curl` e das credenciais via env (`AWS_ACCESS_KEY_ID`/
  `AWS_SECRET_ACCESS_KEY`). Funciona com S3-compatível (MinIO etc.) via
  `S3_ENDPOINT`.
- Bancos relacionais: `fonte tipo: sqlite/postgres/duckdb/mysql/clickhouse`
  é somente leitura (consultas SELECT); gravação via `executar_sql`
  (INSERT/UPDATE/DELETE/DDL, um comando por chamada, sem prepared statements
  nem transações explícitas); Postgres carrega `libpq.so.5`, SQLite
  `libsqlite3.so.0`, DuckDB `libduckdb.so` e MySQL/MariaDB `libmariadb.so.3`
  ou `libmysqlclient.so*` via `dlopen` — precisam estar instalados no sistema.
  No MySQL/MariaDB: sem prepared statements (escaping é responsabilidade do
  autor do SQL), sem TLS explícito (o canal seguro depende da lib cliente
  carregada — contra servidores 8.0+ com `caching_sha2_password`, prefira
  usuário `mysql_native_password` ou SSL fora do escopo), consulta por
  conexão. No ClickHouse: HTTP nativo pelo cliente genérico (sem `dlopen`),
  resposta em `FORMAT JSONEachRow` anexado automaticamente (com `FORMAT`
  próprio no SQL a resposta ainda é parseada linha a linha como NDJSON),
  timeout de 60s, sem UPDATE transacional (`ALTER ... UPDATE` é assíncrono),
  auth por userinfo da URL ou env `CLICKHOUSE_USER`/`CLICKHOUSE_PASSWORD`.
- Redis: TLS via `rediss://` ou `{tls: verdadeiro}`, timeout fixo de 5s.
  AUTH via userinfo da URL (`redis://:senha@host`) ou opção `senha:`; SELECT
  via path numérico (`redis://host:6379/2`) ou opção `banco:`. Sem
  pub/sub, streams, scripts Lua nem conexões persistentes/reconnect —
  `redis_executar` cobre comandos avulsos e `redis_lote` roda um pipeline
  de até 10 mil comandos numa única conexão; `ler_redis`/`escrever_redis`/
  `redis_executar` abrem uma conexão por chamada.
- TLS (redis/mongo/kafka): camada mínima em `src/runtime/tls.*` — OpenSSL
  carregado em runtime via `dlopen` (`libssl.so.3`, fallback `libssl.so`, e
  libcrypto correspondente), zero dependência de link. Verificação de
  certificado contra o trust store do sistema + hostname; `TILT_TLS_SKIP_VERIFY=1`
  desliga a verificação (cert auto-assinado em testes — o próprio erro de
  handshake sugere o env). Sem SASL nem client-cert: se o servidor exigir
  autenticação mútua, o handshake falha com erro do OpenSSL. `mongodb+srv://`
  não faz lookup DNS SRV (usa o host como `mongodb://`). Coberto por
  `tests/tls_test.sh` (mock RESP sobre TLS com cert auto-assinado gerado na
  hora com a CLI `openssl`).
- Qdrant: a coleção usa distância Cosine e ids determinísticos derivados do
  id tilt; `buscar` contra Qdrant devolve `id` e `score` (sem o campo
  `texto`, que fica no payload do ponto).
- Weaviate: a classe é criada com `vectorizer: "none"` (o vetor vem pronto do
  `embeddings:`) e o nome deve ser de GraphQL (`[A-Z][_a-zA-Z0-9]*`); a busca
  é GraphQL `nearVector` (cosseno, `score = 1 - distance`) e devolve `id` e
  `score` (sem o `texto`, que fica na propriedade `texto` do objeto); no
  Weaviate real o `id` do objeto deve ser UUID; auth só por env
  `WEAVIATE_API_KEY` (Bearer), sem usuário/senha nem TLS dedicado (HTTP puro).
- Pinecone: data plane apenas — o índice deve já existir na conta (criar
  índice é control plane, fora de escopo); sempre HTTPS; `PINECONE_API_KEY`
  é obrigatória (header `Api-Key`), com erro claro antes da rede quando
  ausente; o score já é similaridade de cosseno (maior = melhor, sem conversão
  como no Weaviate); sem `ensure` de namespace (o upsert cria implicitamente);
  `buscar` devolve `id` e `score`, sem o `texto` (que vai no `metadata.texto`).
- Chroma: HTTP puro, sem auth (Chroma open-source padrão; Chroma Cloud com
  auth/tls fica fora de escopo); a coleção é get-or-create (`POST
  /api/v1/collections` com o nome) e o `id` devolvido endereça add/query;
  a query devolve `distances` (`distance = 1 - cosseno` com `hnsw:space
  cosine`), então o score tilt é `1 - distance`; `buscar` devolve `id` e
  `score`, sem o `texto` (que fica em `metadatas[].texto`/`documents[]`).
- pgvector: exige a extensão `vector` instalada no banco (o Tilt tenta
  `CREATE EXTENSION IF NOT EXISTS vector`, que precisa de privilégio na
  primeira vez); upsert sem prepared statements (escaping manual de
  strings); nome de coleção restrito a `[a-z0-9_]`.
- Streaming com `janela:`: buffer e relógio da última execução ficam só em
  memória (não há repartição de estado entre réplicas nem checkpoint
  distribuído); o offset persiste em `<fonte>.tilt-offset` apenas para janela
  de contagem sobre fonte de arquivo (csv/json) — Kafka, Mongo etc. não têm
  checkpoint local; sem `grupo:` na fonte Kafka ela é relida do início por
  inteiro a cada tick, o que não escala para tópicos grandes (com `grupo:` o
  checkpoint é o offset commitado no broker).
- `--agendar` entra em loop real de agenda, mas o parser cron é numérico
  (sem nomes `jan`/`mon`), os campos dia-do-mês e dia-da-semana combinam por
  E (não pelo OU do cron clássico) e não há persistência de estado entre
  disparos.

## ML / DL

- `pesos: "arquivo"` carrega no formato tilt-pesos (ver guia 04); arquivo
  ausente mantém o init Xavier com `[nota]`.
- `treino` suporta `perda: entropia_cruzada` (com `softmax` final) e
  `perda: quadratica` (regressão escalar); backward completo de `densa`,
  ativações (inclusive `gelu`, com a derivada exata da aproximação usada na
  forward) e `norma_camada` (sem affine).
- `conv2d`/`norma_lote` existem como **operações de tensor** (guia 04):
  `conv2d` com padding válido e `passo:` (stride) 1+; `norma_lote` com
  `eps:`/`em_treino:`. Limites: sem pooling, sem dilation nem padding
  explícito; não são camadas de `modelo`/`treino` (erro claro no `modelo`),
  sem integração com o carregador de pesos tilt-pesos e sem backward.
- GPU: o backend CUDA (`TILT_GPU=auto`) só foi validado em hardware; aqui use
  `TILT_GPU=fake` para exercitar o caminho de dispatch.

## LLM / RAG

- Sem `TILT_LLM`, a chamada real depende do `curl` no `PATH`.
- `indice` roda com `armazenamento: "memoria"` (cosseno local),
  `"qdrant://host:porta/colecao"` (REST via curl), `"pgvector://colecao"`
  (SQL sobre libpq, cosseno `<=>`; a tabela é criada automaticamente e
  `buscar` devolve `{ id, score }`, sem o texto), `"weaviate://host:porta/classe"`
  (REST via curl, GraphQL `nearVector`; `buscar` devolve `{ id, score }`, sem
  o texto; auth por env `WEAVIATE_API_KEY`),
  `"pinecone://host-do-indice/namespace"` (REST via `curl`, sempre HTTPS,
  `POST /query`; `buscar` devolve `{ id, score }`, sem o texto; exige env
  `PINECONE_API_KEY` e índice já criado na conta) e
  `"chroma://host[:porta]/colecao"` (REST via `curl`, HTTP puro, sem auth;
  coleção get-or-create; `buscar` devolve `{ id, score }`, sem o texto; o
  score é `1 - distance` da query do Chroma).
- Os embeddings do modo `mock` são um bag-of-tokens hasheado (16 dimensões) —
  bons para testes determinísticos, não para relevância real.

## Agentes

- O planner usa um protocolo simples (uma linha por turno: `chamar ...` /
  `responder: ...`); LLMs reais podem ignorá-lo — a resposta fora do
  protocolo vira a resposta final, sem garantia de que as ferramentas certas
  foram chamadas. No modo `mock` o planner é determinístico (cada ferramenta
  uma vez, na ordem declarada).
- Supervisor delega por rótulo; um rótulo sugerido pelo LLM que não está em
  `agentes:` é erro de execução (`T901`).

## HTTP

- As rotas executam em paralelo por padrão (pool de `min(4, núcleos)`
  workers; `--threads N` ajusta, `--threads 1` volta ao serial). Rotas que
  tocam o **mesmo** `indice` em memória se serializam por um mutex global do
  índice — para alta concorrência, use Qdrant/pgvector como armazenamento.
- No Linux: epoll + keep-alive + arena por requisição + pool de rotas com
  ordenação por sequência por conexão (M10.2 + paralelismo entregues).
  Em outros sistemas, o servidor é bloqueante, uma conexão por vez,
  `Connection: close`.

## VM / nativo

- A VM cobre `funcao` pura e `pipeline`s no subconjunto (literais incl.
  listas, `para cada`, índice, `contem`); o resto roda no interpretador de
  árvore (`tilt executar --vm` cai por pipeline, transparente).
- `e` / `ou` na VM fazem curto-circuito (desde a Fase 8), com resultado
  sempre `logico`.
- `tilt compilar` cobre o **programa inteiro** dentro do subconjunto da VM:
  `funcao principal` ou pipelines, com texto/decimal/lista e saída idêntica
  ao interpretador (runtime C espelhando `value.cpp`; teste `native`
  diferencial). Fora do subconjunto (builtins como `ler_csv`, interpolação,
  membros, `agenda:`/`ao_falhar:`) rejeita com mensagem clara.
- Backends de codegen nativo: **x86-64** e **ARM64 (AArch64)**, o mesmo
  subconjunto nos dois (`--arch x86_64|arm64`, `auto` = host). O backend
  ARM64 emite ELF/AAPCS (validado por geração + montagem cross no teste
  `native_arm64`; a execução sob `qemu-aarch64` no ctest depende de toolchain
  cross + qemu instalados — sem ela, a validação end-to-end fica para CI /
  máquina ARM). Mach-O (macOS) e PE/COFF (Windows) ficam fora: o codegen
  é ELF-only. **JIT** (compilação em runtime, sem passar por `.s`+`cc`)
  segue como evolução futura.

## Stdlib

A stdlib instalada com o tilt (`<prefixo>/share/tilt/stdlib`, resolução em
"Importar" no guia 01) cobre em 1ª passada:

- **`io`** — `juntar_caminhos`, `existe_arquivo`, `ler_json_seguro`,
  `salvar_json`, todos sobre os builtins de dados existentes. De fora:
  leitura de arquivo bruto como texto, tamanho em bytes (`stat`), listar e
  remover arquivos — não há builtin para isso no runtime.
- **`rede`** — `get_json`/`post_json` reais, sobre os builtins de HTTP
  genérico `http_get_json`/`http_post_json` (cliente mínimo via subprocesso
  `curl`, timeout padrão de 30s, só JSON). Erros de transporte, HTTP >= 400 e
  JSON inválido abortam — use `tentar`/`capturar` para tratá-los como valor.
  De fora: outros verbos (PUT/PATCH/DELETE), corpo bruto (não-JSON),
  streaming e controle fino de timeout.
- **`nn`** — `linear`, `atencao`, `atencao_causal`, `feedforward`,
  `bloco_atencao`, `norma_camada`, compostos sobre os ops de tensor
  (`.matmul`, `.softmax`, `.norma_camada()` etc.). Limites: forward-only
  (treino continua em `modelo`/`treino`), sem `sqrt` na linguagem — a escala
  da atenção (`1/raiz(d_k)`) é parâmetro — e sem máscaras de atenção
  arbitrárias (a causal é montada como listas aninhadas, o que exigiu `'+'`
  concatenando listas).

## Plataforma

- `tilt compilar` gera ELF para x86-64 e ARM64 (AArch64); em outras
  arquiteturas de host não há backend (`--arch` rejeita com erro claro) e o
  teste `native` é pulado fora de x86-64.
- Binário estático de libstdc++ só no Linux (no macOS usa a libc++ do sistema).
- **Port Windows (1ª passada)**: o projeto compila no MSVC/MinGW via CI
  (job `windows` em `.github/workflows/ci.yml`). A camada de compatibilidade
  vive em `src/runtime/compat.*`: sockets POSIX viram Winsock2
  (`WSAStartup` no CLI), `dlopen` vira `LoadLibrary` (nomes de DLL:
  `libssl-3-x64.dll`, `sqlite3.dll`, `libpq.dll`, `duckdb.dll`,
  `libmariadb.dll`, `zlib1.dll`, `nvcuda.dll`),
  `epoll` do servidor HTTP vira um event loop com `select()` (sem o caminho
  paralelo de workers — `--threads N` é serial no Windows por enquanto),
  cores do terminal ficam desligadas. Limites atuais do port:
  - HTTP externo (S3, LLM, Qdrant, Iceberg REST) continua dependendo do
    binário `curl` — no Windows, `curl.exe` do sistema (Windows 10+), mas o
    quoting de argumentos segue o padrão shell POSIX (aspas simples), o que
    o `cmd.exe` não interpreta; trate esses conectores como não validados
    no Windows nesta fase.
  - TLS carrega OpenSSL via DLL (`libssl-3-x64.dll`/`libcrypto-3-x64.dll`)
    no `PATH`; sem elas, `rediss://`/`mongodb+srv://`/kafka TLS erros claros.
  - A suíte `ctest` é em shell script e só roda em Linux/macOS — o job
    Windows valida build + smoke (`versao`, `checar`, `executar`).
