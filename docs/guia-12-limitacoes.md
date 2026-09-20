# 12 — Limitações (1ª passada)

Cada marco `M0`–`M12` foi entregue em "1ª passada": o caminho principal
funciona, mas há bordas conhecidas. Lista do que **ainda não** funciona.

## Sintaxe / parser

- Sem palavras reservadas (Marco 3 / C3): `e` / `ou` / `nao` / `contem` valem
  como nomes de variável, parâmetro e loop var — o contexto decide (operador
  em posição de operador, nome em posição de nome).
- Parâmetros compostos em `funcao` (Marco 3 / C3): `nome[]` ou
  `nome[]: <tipo>` (opcional), `mapa` como tipo base; `Arg.optional` é
  metadado do parser. A aridade de funções de usuário é validada pelo checker:
  argumentos faltantes são erro; argumentos excedentes são aceitos apenas no
  formato de chamada entre parênteses, conforme a regra de chamada da Tilt.

## Semântica

- `tilt checar` resolve nomes dentro de `passos:` / `executar:` (`T030`):
  escopo global mais variáveis implícitas (`linha`, `entrada`, `epoca`,
  `metricas`, `passo`, `resultado`) e campos de `entrada:`. Nomes fora
  disso são reportados.
- O solver de formas (`T012`) cobre a cadeia `densa`/`linear`/`residual` nos `modelo`s
  (propaga a última dimensão a partir da anotação `entrada: tensor[...]` e
  rejeita `linear: [a, b]` com `a` incompatível) e, nos corpos de
  `funcao`/`pipeline`/`servico`, operações de tensor com formas literais ou
  anotadas: `conv2d` (rank 4, canais, núcleo vs. entrada, `passo:`),
  `norma_lote` (rank >= 2), `softmax`/ativações/`norma_camada` (preservadas),
  `reformar` (n. de elementos, com `_` inferido), `transposta` (2D),
  `matmul` (2D e batched ND), broadcast elementwise (NumPy: 1 expande) e
  `atencao(q, k, v, escala)` (bare ou `nn.atencao`). Dimensões `_`
  (simbólicas, `-1`) são compatíveis com tudo e se propagam; contratos de
  funções locais com entrada/retorno tensor instanciam `_` no chamador, e
  `m.campo = tensor` preserva a forma conhecida do campo. Formas
  definidas em todos os ramos de `se`/`senao` também são fundidas; dimensões
  divergentes viram `_` e seguem para `atencao`/`conv2d` sem falso positivo. Incompatível
  evidente continua sem veredito (runtime decide). Fora do
  solver: formas através de chamadas de `funcao` genéricas e operações cujo
  rank não é conhecido —
  nesses casos a validação de dimensão continua acontecendo em runtime. No
  runtime, `_` em forma avaliada falha com mensagem própria, exceto em
  `reformar([...])` (inferido) e anotação/`checar`.
- Não há inferência completa de tipos: o que `checar` cobre hoje (`T011`) é
  o subconjunto evidente — operadores aritméticos/comparação com ambos os
  lados de tipo conhecido (rejeita `lista + 1`, `"a" - 1`, `"a" < 1`, mas
  aceita `texto + numero` e `"a" < "b"`, que o runtime suporta), builtins com
  aridade e 1º/2º argumento tipados (ex.: `tamanho 42`, `ler_csv 123`;
  `executar_sql` valida ainda o 3º (`params` deve ser lista) e `transacao`
  exige `(texto, lista)`),
  métodos/campos de receiver conhecido (ex.: `"abc".matmul`, `t.filtrar` em
  tensor, `5.maiusculas`), retorno de `funcao` (anotado `-> T` ou inferido do
  corpo por unanimidade dos `retornar`), campos de mapas (literais e
  variáveis com literal, com erro de campo inexistente), índice em lista de
  elemento homogêneo e agregações (`somar`/`min`/`max` refinam pelo elemento;
  `media` é decimal). Fluxo condicional funde tipos definidos em todos os
  ramos de `se`/`senao` (com promoção `inteiro` → `decimal`); caminhos
  parcialmente definidos continuam desconhecidos. Por entidade — fora daí o tipo
  vira "desconhecido" e segue sem verificação: campos de `tipo` Registro, campos dinâmicos de tabelas e
  `verificar`/`ao_falhar`.

## Dados

- Parquet é nativo (reader/writer próprio, zero dependências de link): a
  escrita é PLAIN com compressão **gzip** (padrão), **snappy** (`codec:
  "snappy"` — compressor literal-only, sem ganho de espaço mas interoperável),
  páginas DATA_PAGE **v1** (padrão) ou **v2** (`paginas: "v2"`), um row group
  por arquivo, com colunas REQUIRED ou OPTIONAL (nulos via definition levels
  RLE),   **listas de escalares** (anotação LIST, elemento Nulo vira OPTIONAL —
  Marco 2 / B3), **structs** (Fase 12-5a), **listas aninhadas**
  (`list<list<...>>`, Marco 2 / B2a, nulos em todos os níveis),
  **3 níveis de lista** (Fase 12-5a.1, nulos/vazios em todos os níveis) e
  **listas de structs** (Marco 2 / B2b, elemento Nulo vira OPTIONAL;
  campos-escalares **e campos-lista** nos elementos, Fase 12-5a.1) — tudo
  validado com pyarrow nos dois sentidos — e estreitamento opt-in `tipos: {col:
  "int32"|"float"}` (Marco 1 / B1, com anotação INTEGER) e dictionary
  encoding automático com fallback (Marco 2 / B4, `dicionario: falso`
  desliga). A leitura cobre
  múltiplos row groups, campos REQUIRED/OPTIONAL/
  REPEATED (listas aninhadas e de structs, inclusive o element OPTIONAL que o
  pyarrow grava, com Nulo preservado), tipos
  **INT32/INT64/FLOAT/DOUBLE/BOOLEAN/BYTE_ARRAY/FIXED_LEN_BYTE_ARRAY/INT96**,
  lógicos **UTF8/STRING/INTEGER/DATE/TIME/TIMESTAMP/DECIMAL** (data/hora/
  timestamp viram texto ISO, decimal vira decimal) e dictionary pages com
  encoding PLAIN ou PLAIN_DICTIONARY, páginas v1 e
  v2, PLAIN e DICTIONARY (PLAIN_DICTIONARY/RLE_DICTIONARY) e os codecs
  gzip/deflate (zlib via `dlopen`), **snappy** (codec próprio) e **ZSTD**
  (libzstd via `dlopen`). Ainda fora do subconjunto: 4+ níveis de lista e
  criptografia Parquet.
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
  `__HIVE_DEFAULT_PARTITION__` nem escaping) e checkpoint tilt-native a cada
  10 versões (`<v>.checkpoint.parquet` + `<v>.checkpoint.meta.json` em
  _delta_log, ignorados por leitores externos) mais leitura do checkpoint
  padrão (`_last_checkpoint` + `<v>.checkpoint*.parquet` no schema oficial,
  honrado como base — tabelas com checkpoint de Spark/delta-rs leem rápido);
  a leitura
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
  Hadoop continua, byte a byte. Na direção inversa, `tilt servir-catalogo`
  (fase 30) expõe as tabelas Hadoop locais como **catálogo REST server
  read-only** (subconjunto de leitura v1: config/namespaces/tables/loadTable +
  endpoint de arquivos com proteção contra path traversal; createTable/commit →
  501) — o metadata servido reescreve as locations para URLs do servidor, mas
  para o Spark/Hadoop (cujo `fs.http` reporta length -1, rejeitado pelo leitor
  Avro do Iceberg) há o modo `--sem-reecrita-manifests`, em que manifest lists,
  manifests e data files seguem `file://` absolutos (o
  `tests/spark_catalog_test.sh` monta o diretório no mesmo path dentro do
  container). Demais limites: a
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
   ["c1", "c2"]`, field-ids 1000, 1001, ...) com transform `identity`
   (layout `<c1>=<v1>/<c2>=<valor>/00000-0-<uuid>.parquet` sem as colunas no
   parquet, record `partition` no manifest e colunas reidratadas na leitura com
   conversão de tipo) e **`bucket[N]`** (Fase 12-5a: `particionar_por:
   ["bucket[4](id)"]`, murmur3 da spec, campo `id_bucket_4` int, coluna de
   origem mantida no parquet, poda por hash + residual exato — validado com
   pyiceberg e referência mmh3), **structs aninhados** (Marco 2 / B5: `mapa`
   vira STRUCT com field-ids em profundidade e ids nos grupos do footer;
   evolução adiciona (sub)coluna optional no fim com id novo, com projeção
   de nulo em arquivos antigos — validado com pyiceberg), com **pruning**
   em `ler_iceberg ... onde: {...}` (igualdade; predicados em coluna de partição pulam data files
   inteiros pelos manifests, o resto filtra linhas). Os transforms `truncate`,
   `year`, `month`, `day` e `hour` também participam da poda por valor.
   **Deletes (Fase 12-5a)**: `apagar_iceberg` (position e
   equality) + leitura filtrada; pyiceberg aplica os position deletes do
   tilt (equality deletes o próprio pyiceberg ainda não suporta — upstream).
   Os manifest lists carregam `partitions` com `contains_null`, `lower_bound` e
   `upper_bound` por campo, além de `sequence_number`/`min_sequence_number`;
   snapshots e entradas de manifest também recebem sequence numbers reais da
   spec v2 (validado com pyiceberg). Mas: valor nulo em coluna de partição,
   valores com `/` e coluna repetida não são suportados (erro claro, sem
   escaping). A estrutura escrita (metadata, manifest list, manifest e
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
  com produce idempotente (Marco 1: `InitProducerId` + Produce v3 RecordBatch
  com sequência por partição e dedup no retry; fallback v1 em broker 0.9-era)
  + retry (3x em 5/6/7) + `chave:`; transações multi-partição
  (`AddPartitionsToTxn/EndTxn`) ficam para depois. Consumer groups com rebalanceamento `"roundrobin"` real (o líder
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
  (INSERT/UPDATE/DELETE/DDL, um comando por chamada, com `?` posicionais via
  lista `params` opcional), leitura parametrizada via `consultar_sql`
  (mesma ligação, devolve tabela) e   `transacao` (BEGIN/COMMIT numa única conexão dedicada — fora do pool,
  ROLLBACK com o índice do passo; ClickHouse sem transações — erro claro);
  postgres/mysql/duckdb têm pool por (backend, url) nos statements avulsos
  (`executar_sql`/`consultar_sql`: até 8 ociosas por chave, validadas com
  PQstatus/mysql_ping no checkout; `TILT_SQL_POOL=0` desliga,
  `TILT_SQL_POOL_MAX` ajusta o teto, `TILT_SQL_POOL_DEBUG=1` loga
  hit/miss/stale/discard; BEGIN/START/SET no início do SQL não volta ao
  pool). sqlite (open barato) e clickhouse (HTTP) seguem uma conexão por
  chamada, sem pool;
  Postgres carrega `libpq.so.5`, SQLite
  `libsqlite3.so.0`, DuckDB `libduckdb.so` e MySQL/MariaDB `libmariadb.so.3`
  ou `libmysqlclient.so*` via `dlopen` — precisam estar instalados no sistema.
  Ligação: postgres `PQexecParams` (`?`→`$N`), sqlite `sqlite3_bind_*`,
  duckdb prepared (`duckdb_prepare`; lib antiga sem os símbolos falha com erro
  claro em vez do caminho legado), mysql prepared server-side (`mysql_stmt_*`
  com `MYSQL_BIND` espelhado — layout comum a libmysqlclient e libmariadb,
  validado contra as duas; tudo ligado como texto com coerção no servidor,
  leitura como bytes com `fetch_column` em truncamento), clickhouse `{pN:Tipo}` via query params
  (nulo→`NULL` inline). No MySQL/MariaDB: sem TLS explícito (o canal seguro depende da lib cliente
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
  hora com a CLI `openssl`). Quando OpenSSL ou uma DLL/SO estiver ausente, o erro
  preserva os nomes tentados e o detalhe do carregador para orientar a instalação.
- Qdrant: a coleção usa distância Cosine e ids determinísticos derivados do
  id tilt; `buscar` contra Qdrant devolve `id`, `texto` e `score` (ambos vêm do
  payload do ponto; pontos de versões antigas, sem `tilt_id`, devolvem o UUID).
- Weaviate: a classe é criada com `vectorizer: "none"` (o vetor vem pronto do
  `embeddings:`) e o nome deve ser de GraphQL (`[A-Z][_a-zA-Z0-9]*`); a busca
  é GraphQL `nearVector` (cosseno, `score = 1 - distance`) e devolve `id`,
  `texto` (propriedade `texto` do objeto) e `score`; a gravação faz GET e depois
  PUT (existe) ou POST (novo), pois o PUT de um id novo falha; no
  Weaviate real o `id` do objeto deve ser UUID; auth só por env
  `WEAVIATE_API_KEY` (Bearer), sem usuário/senha nem TLS dedicado (HTTP puro).
- Pinecone: data plane apenas — o índice deve já existir na conta (criar
  índice é control plane, fora de escopo); sempre HTTPS; `PINECONE_API_KEY`
  é obrigatória (header `Api-Key`), com erro claro antes da rede quando
  ausente; o score já é similaridade de cosseno (maior = melhor, sem conversão
  como no Weaviate); `ensure` consulta `describe_index_stats` e valida o namespace antes de buscar; o upsert continua criando-o implicitamente;
  `buscar` devolve `id`, `texto` (de `metadata.texto`) e `score`.
- Chroma: HTTP puro, sem auth (Chroma open-source padrão; Chroma Cloud com
  auth/tls fica fora de escopo); a coleção é get-or-create (`POST
  /api/v1/collections` com o nome) e o `id` devolvido endereça add/query;
  a query devolve `distances` (`distance = 1 - cosseno` com `hnsw:space
  cosine`), então o score tilt é `1 - distance`; `buscar` devolve `id` e
  `texto` (de `documents[]`) e `score`.
- pgvector: exige a extensão `vector` instalada no banco (o Tilt tenta
  `CREATE EXTENSION IF NOT EXISTS vector`, que precisa de privilégio na
  primeira vez); upsert sem prepared statements (escaping manual de
  strings); nome de coleção restrito a `[a-z0-9_]`.
- CI real dos conectores vetoriais: o job vector_connectors provisiona
  Qdrant, Weaviate, Chroma e pgvector em containers pinados e executa
  tests/vector_connectors_real_test.sh; fora desse job, o teste vector_real
  pula quando os serviços não estão disponíveis.

- Streaming com `janela:`: buffer fica em memoria; o offset persiste em
  `<fonte>.tilt-offset` para fonte de arquivo (csv/json) — com
  `TILT_CHECKPOINT_DIR` o arquivo mora no diretorio compartilhado, num objeto
  `s3://` ou num topico `kafka:` (mapa inteiro por save, last-wins) e janelas
  de tempo/throttle tambem persistem `last_run`; eleicao de lider por lease em
  arquivo (`TILT_LEADER_LEASE`, `TILT_LEADER_TTL`) garante escritor unico no
  `--agendar` multi-replica.
  Kafka, Mongo etc. sem `grupo:` nao têm checkpoint local; sem `grupo:` na fonte Kafka ela é relida do início por
  inteiro a cada tick, o que não escala para tópicos grandes (com `grupo:` o
  checkpoint é o offset commitado no broker).
- `--agendar` entra em loop real de agenda, mas o parser cron é numérico
  (sem nomes `jan`/`mon`), os campos dia-do-mês e dia-da-semana combinam por
  E (não pelo OU do cron clássico). Janelas sobre arquivos persistem offset,
  buffer pendente e `last_run` entre disparos; conectores sem checkpoint de
  grupo continuam sujeitos às limitações descritas nas seções próprias.

## ML / DL

- `experimento` é de 1ª passada (guia 04): `regressao_linear` (equações
  normais + crista), `regressao_logistica` binária e multinomial (GD em
  lote), `knn` (classificação e regressão), `kmeans` (Lloyd, sem `alvo:`),
  `floresta_aleatoria`, `gradiente_impulsionado` e `svm`. Limites: sem
  busca de hiperparâmetros, `pre_processar` só `um_de_n`/`padronizar`/
  `imputar` (sintaxe `- chave: [cols]`; o `->` do esboço original não
  parseia), `f1` ponderado pelo suporte, `registrar_em: mlflow://` envia
  parâmetros e métricas ao Tracking REST do MLflow.
- `pesos: "arquivo"` carrega no formato tilt-pesos (ver guia 04); arquivo
  ausente mantém o init Xavier com `[nota]`. `carregar_pesos` faz o mesmo em
  tempo de execução; `exportar_onnx` exporta o modelo para ONNX opset 20
  (Gemm + ativações + Softmax + LayerNormalization + Conv +
  BatchNormalization + MaxPool + Flatten + RNN/LSTM/GRU + residual). A camada `incorporacao` ainda nao e exportavel para ONNX;
  use pesos/treino nativos ou GGUF.
- `treino` suporta `perda: entropia_cruzada` (com `softmax` final) e
  `perda: quadratica` (regressão escalar); backward completo de `densa`,
  ativações (inclusive `gelu`, com a derivada exata da aproximação usada na
  forward), `norma_camada` (sem affine), `conv2d`, `norma_lote`,
  `agrupamento_max`, `achatar`, `residual` e `recorrente` (RNN/LSTM/GRU com BPTT em CPU) (CNN de brinquedo em CPU, com mini-lotes).
- `conv2d` e `norma_lote` existem como **operações de tensor** (guia 04);
  `incorporacao`, `recorrente`, `conv2d` e `norma_lote` existem como camadas de `modelo`/`treino` (`incorporacao: [vocabulario, dimensao]`,
  `conv2d: [C_saida, C_entrada, KH, KW, passo, padding, dilatacao]`
  com os três últimos elementos opcionais, `norma_lote`, `agrupamento_max:
  [janela]`/`[janela, passo]`, `achatar`; CNN exige `entrada: tensor[...]`
  completa): `incorporacao` usa índices inteiros e produz `[N, T, D]`; `recorrente` recebe `[N, T, F]` e produz `[N, H]`; `residual` recebe vetor 1D de largura conhecida e preserva `[D]`; `conv2d` tem
  padding/dilation explícitos e passo 1+ (com viés); `norma_lote`
  com `eps:`/`em_treino:` nas ops e gama/beta +   média/variância correntes nas
  camadas. `treino` roda em lote cheio por default, com `lote:` (mini-lotes
  embaralhados por época), `semente:` (init + embaralhamento reproduzíveis),
  `checkpoint:`/`a_cada:` (JSON tilt-checkpoint com pesos, momentos do Adam
  e época) e `retomar:` (continuação bit-idêntica), `agendador:`
  (`{ tipo: cosseno }` ou `{ tipo: degrau, a_cada:, fator: }`),
  `validacao:` (fração) + `parar_cedo:` (`N` ou
  `{ paciencia:, melhorar_min: }`, restaura os melhores pesos) e `busca`
  em grade (`modelo:`, `grade:`, `criterio: perda|acuracia`, máx. 64
  combinações, melhor fica no modelo). `carregador ..., fluxo: verdadeiro` treina CSV ou Parquet grande em blocos
  (`bloco:`, default 1024; Parquet usa row groups) sem materializar — bit-idêntico
  ao RAM. `exportar_gguf` grava GGUF v3 (só escrita) e `salvar_pesos`/`carregar_pesos` aceitam Safetensors F32 e ONNX; ONNX cobre as camadas exportáveis e a camada `incorporacao` continua sem suporte. Limites: fluxo só modelo 2D;
  sem AMP.
- GPU: o backend CUDA (`TILT_GPU=auto`) ainda não foi validado em hardware
  CUDA real; aqui use `TILT_GPU=fake` para exercitar o caminho de dispatch.

## LLM / RAG

- Sem `TILT_LLM`, a chamada real depende do `curl` no `PATH`.
- Segredos (chave de API do LLM, headers de qualquer `http_*`/S3/Elasticsearch/
  Pinecone, userinfo da URL) vão para o `curl` por um arquivo de configuração
  `-K` temporário (0600, removido ao fim da chamada), nunca pelo argv — que
  outros usuários da máquina leem em `ps`/`/proc`. O `curl` ainda é iniciado
  por shell (`popen`); só caminhos de arquivos temporários passam pela linha
  de comando. Coberto por `tests/curl_secrets_test.sh`.
- Robustez do cliente (guia 05): `tempo_limite:` (segundos por tentativa,
  default 60, via `--max-time`), `tentativas:` (default 3, retry com backoff
  1s/2s/4s… teto 15s em erro de transporte, 429 e 5xx; 4xx falha rápido),
  `reserva: [outro_llm]` (fallback em ordem, sem cadeia) e `teto_tokens:`
  (barreira no acumulado entrada+saída por `llm` antes de cada chamada).
  `perguntar` devolve `{texto, modelo, tokens: {entrada, saida}}` (tokens do
  `usage` da API; no mock, heurística chars/4). O retry respeita `Retry-After`
  numérico ou HTTP-date em respostas 429 (limitado a 300s). Ainda não há cache
  de respostas nem retry em streaming (timeout vale para o SSE inteiro).
- `indice` roda com `armazenamento: "memoria"` (cosseno local),
  `"qdrant://host:porta/colecao"` (REST via curl), `"pgvector://colecao"`
  (SQL sobre libpq, cosseno `<=>`; a tabela é criada automaticamente e
  `buscar` devolve `{ id, texto, score }`), `"weaviate://host:porta/classe"`
  (REST via curl, GraphQL `nearVector`; `buscar` devolve `{ id, texto, score }`; auth por env `WEAVIATE_API_KEY`),
  `"pinecone://host-do-indice/namespace"` (REST via `curl`, sempre HTTPS,
  `POST /query`; `buscar` devolve `{ id, texto, score }`; exige env
  `PINECONE_API_KEY` e índice já criado na conta) e
  `"chroma://host[:porta]/colecao"` (REST via `curl`, HTTP puro, sem auth;
  coleção get-or-create; `buscar` devolve `{ id, texto, score }`; o
  score é `1 - distance` da query do Chroma).
- Os embeddings do modo `mock` mantêm 16 dimensões e combinam tokens hasheados com trigrams
  com padding de borda e normalização L2; continuam sendo apenas um mock determinístico, não relevância real.
- `avaliacao` é de 1ª passada (guia 05): `dados:` inline/bloco/caminho,
  `executar:` por caso com `caso` + `retornar`, métricas `exata`/`contem`/
  `regex`/`tolerancia`/`juiz` (todas precisam passar por caso), gate no
  `limiar:`, `amostra:` + `semente:` determinísticos e `registrar_em:` local ou via MLflow REST. Limites: juiz sem cadeia de pensamento; voto multi-juiz usa maioria ou unanimidade,
  amostra por contagem, opcionalmente estratificada proporcionalmente por campo; não há frações ou pesos manuais; o MLflow ainda não publica artefatos ou detalhes de cada caso.

## Agentes

- O planner usa um protocolo simples (uma linha por turno: `chamar ...` /
  `responder: ...`); LLMs reais podem ignorá-lo — a resposta fora do
  protocolo vira a resposta final, sem garantia de que as ferramentas certas
  foram chamadas. No modo `mock` o planner é determinístico (cada ferramenta
  uma vez, na ordem declarada).
- Supervisor delega por rótulo; um rótulo sugerido pelo LLM que não está em
  `agentes:` é erro de execução (`T901`).
- Serviços HTTP podem restringir chamadas de ferramentas com `ferramentas: [...]`; sem essa lista, rotas mantêm o comportamento aberto.
- Ferramentas validam campos obrigatórios, campos desconhecidos e tipos escalares/listas/mapas nas chamadas; registros nomeados são tratados como mapas e não têm validação recursiva de esquema.

## HTTP

- As rotas executam em paralelo por padrão (pool de `min(4, núcleos)`
  workers; `--threads N` ajusta, `--threads 1` volta ao serial). Rotas que
  tocam o **mesmo** `indice` em memória se serializam por um mutex global do
  índice — para alta concorrência, use Qdrant/pgvector como armazenamento.
- Observabilidade opt-in no `servico`: `saude: verdadeiro` (GET /saude) e
  `metricas: verdadeiro` (GET /metricas com totais/erros e latência monotônica
  em microssegundos por rota), além de `/metricas/prometheus` em formato
  Prometheus. O log de cada requisição é JSON compacto com `trace_id`.
- No Linux: epoll + keep-alive + arena por requisição + pool de rotas com
  ordenação por sequência por conexão (M10.2 + paralelismo entregues).
  Em outros sistemas, o servidor é bloqueante, uma conexão por vez,
  `Connection: close`.

## VM / nativo

- A VM cobre `funcao` pura e `pipeline`s no subconjunto (literais incl.
  listas, `para cada`, índice, `contem`, interpolação `{{nome}}` sobre locais,
  acesso a campo — mapa/tabela, `.tamanho`, props de tensor, `?.` — e
  `ler_csv` de 1 argumento); o resto roda no interpretador de
  árvore (`tilt executar --vm` cai por pipeline, transparente). Chunks vão
  para `<fonte>.tiltc` (SHA do fonte; `TILT_VM_NOCACHE=1` desliga).
- `e` / `ou` na VM fazem curto-circuito (desde a Fase 8), com resultado
  sempre `logico`.
- `tilt compilar` cobre o **programa inteiro** dentro do subconjunto da VM:
  `funcao principal` ou pipelines, com texto/decimal/lista e saída idêntica
  ao interpretador (runtime C espelhando `value.cpp`; teste `native`
  diferencial). Fora do subconjunto (`ler_csv`, membros/GetField,
  `agenda:`/`ao_falhar:`) rejeita com mensagem clara — o validador do
  codegen é fail-closed (allow-list de ops + nomes de CallFunc).
- Backends de codegen nativo: **x86-64** e **ARM64 (AArch64)**, o mesmo
  subconjunto nos dois (`--arch x86_64|arm64`, `auto` = host). O backend
  ARM64 emite ELF/AAPCS; o teste `native_arm64` gera, monta e executa sob
  `qemu-aarch64` no job `arm64_codegen` do CI, que instala a toolchain cross.
  Localmente, sem `gcc-aarch64-linux-gnu` + QEMU, ele fica limitado à geração
  e montagem quando o assembler estiver disponível. Mach-O (macOS) e PE/COFF (Windows) ficam fora: o codegen
  é ELF-only. O JIT (`tilt executar --jit`) emite x86-64 diretamente em memória para o subconjunto inteiro (constantes, locais, aritmética, comparações, condicionais, laços e `imprimir`); decimal, texto, listas, membros e chamadas caem automaticamente para a VM. Arquiteturas sem backend JIT usam o mesmo fallback.

## Stdlib

**Funções embutidas** (`src/runtime/stdlib.cpp`, sem `importar`): matemática
(`raiz`, `abs`, `exp`, `logaritmo`, `potencia`, `piso`, `teto`, `arredondar`,
`seno`, `cosseno`, `tangente`, `pi`), conversões (`inteiro`, `decimal`, `texto`,
`logico`, `tipo_de`), texto (`maiusculas`, `minusculas`, `aparar`, `substituir`,
`comeca_com`, `termina_com`, `juntar`, `regex_casa`, `regex_extrair`,
`regex_substituir` — ECMAScript), listas e mapas (`ordenar`, `unicos`, `reverso`,
`zip`, `enumerar`, `chaves`, `valores`), tempo em UTC (`agora`, `timestamp`,
`formatar_data`, `dormir`), arquivos de texto (`ler_texto`, `escrever_texto`,
`anexar_texto`, `listar_arquivos`, `remover_arquivo`) e `sha256`,
`base64_codificar`/`base64_decodificar`, `json_texto`/`json_ler`. Função do
usuário com o mesmo nome tem prioridade. Limites: `maiusculas`/`minusculas`
só ASCII; datas só UTC (sem fuso); `ordenar` compara só números com números ou
textos com textos.


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
  cores ANSI via VT processing no console, `in_path` com `PATHEXT`.
  Limites atuais do port:
  - HTTP externo (S3, LLM, Qdrant, Iceberg REST) depende do `curl.exe` do
    sistema (Windows 10+); os argumentos usam quoting `cmd.exe`
    (`tilt_win_quote`, regra CommandLineToArgvW — teste `quote_test.sh`),
    mas como não há runner Windows local, trate como em validação pelo CI.
    Residual: pares `%...%` sofrem expansão do `cmd` (URLs com dois escapes
    `%NN` podem corromper; só trocando o spawn por `CreateProcess`).
  - TLS carrega OpenSSL via DLL (`libssl-3-x64.dll`/`libcrypto-3-x64.dll`)
    no `PATH`; sem elas, `rediss://`/`mongodb+srv://`/kafka TLS erros claros.
  - O CTest no Windows registra `windows_functional`, executando
    `tests/windows_functional.ps1` nativamente em PowerShell e cobrindo
    interpretador, VM, JIT/fallback, exemplos ETL e rotas HTTP versionadas.
    Testes de conectores que dependem de shell, Python ou servidores locais
    continuam condicionados a Linux/macOS.
  - O job `connectors` do CI instala PostgreSQL, MariaDB e libmariadb, baixa
    uma libduckdb oficial fixada e roda os testes reais de PostgreSQL, MySQL,
    DuckDB e ClickHouse; o ambiente local pode continuar pulando esses testes
    quando os servidores ou bibliotecas não estiverem instalados.
