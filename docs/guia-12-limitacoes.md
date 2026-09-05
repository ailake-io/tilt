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
- O solver de formas (`T012`) cobre a cadeia `densa`/`linear` — propaga a
  dimensão corrente a partir da anotação `entrada: tensor[...]` e rejeita
  `linear: [a, b]` com `a` incompatível. `conv2d`, `norma_lote` e
  `norma_camada` ficam fora do solver.
- Não há inferência completa de tipos: anotações são validadas como
  contratos, mas os tipos não são propagados entre expressões.

## Dados

- Parquet é nativo (reader/writer próprio, zero dependências de link): a
  escrita é PLAIN, sem compressão, um row group por arquivo, com colunas
  REQUIRED ou OPTIONAL (nulos via definition levels RLE). A leitura cobre
  múltiplos row groups, campos REQUIRED/OPTIONAL, páginas PLAIN e DICTIONARY
  (PLAIN_DICTIONARY/RLE_DICTIONARY) e gzip/deflate (zlib via `dlopen`;
  snappy e demais codecs levantam erro claro). DATA_PAGE_V2, campos REPEATED
  e tipos físicos fora de BOOLEAN/INT64/DOUBLE/BYTE_ARRAY ainda não são lidos.
- Delta Lake é mínimo: `escrever_delta` sobrescreve a tabela (recria a versão
  0); o append existe via `anexar_delta` (nova versão por commit atômico de
  `rename`, validação de schema por nome com evolução limitada — ver abaixo —,
  single-writer — sem locks/optimistic concurrency). Partições hive-style
  existem para **uma coluna**
  (`particionar_por:`, layout `<col>=<valor>/part-NNNNN.parquet`, coluna
  reidrata na leitura), mas: valor nulo em coluna de partição e valores com
  `/` não são suportados (erro claro, sem `__HIVE_DEFAULT_PARTITION__` nem
  escaping), a leitura não filtra por diretório de partição (lê tudo e
  reidrata) e não há checkpoints; a leitura herda o subconjunto do Parquet
  acima. **Evolução de schema (fase 27)**: o append aceita colunas a mais —
  toda coluna antiga presente (ordem livre), coluna nova entra nullable no
  fim do `schemaString` com `metaData` novo no commit; arquivos antigos ficam
  sem a coluna e a leitura projeta nulo (union-by-name). Remover coluna ou
  mudar o tipo de uma existente → erro claro.
- Iceberg é de 1ª passada: catálogo só **Hadoop** (diretório local — sem
  REST/JDBC), codec Avro "null" apenas, a leitura cobre o mesmo subconjunto do
  Parquet acima (tabelas de outros escritores sem garantia além dele) e
  single-writer (sem locks nem optimistic concurrency);
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
  Partições existem para **uma coluna** e só com transform `identity`
  (`particionar_por:`, layout `<col>=<valor>/00000-0-<uuid>.parquet` sem a
  coluna no parquet, record `partition` no manifest e coluna reidratada na
  leitura com conversão de tipo), mas: valor nulo em coluna de partição e
  valores com `/` não são suportados (erro claro, sem escaping), a leitura não
  filtra por partição (lê tudo e reidrata), não há partitions summary nos
  manifests e data sequence numbers são sempre 0. A estrutura escrita
  (metadata, manifest list, manifest e parquet com field-ids) carrega no
  **pyiceberg**.
- Todos os conectores planejados rodam — a lista de stubs de conectores está
  vazia. CSV, JSON, Parquet, Delta, Iceberg, SQLite, Postgres, Redis, Kafka,
  MongoDB, Qdrant, pgvector e S3 rodam.
- MongoDB (`mongo_inserir`/`mongo_buscar`/`mongo_atualizar`/`mongo_deletar`/
  `mongo_criar_indice`): BSON + OP_MSG próprios com CRUD básico completo —
  restam: sem `aggregate`, sem `$unset`/`$inc`/demais operadores de update
  (só `$set`), sem índices de texto/TTL, filtro só por igualdade exata
  top-level (combinado por E), `mongo_deletar` remove sempre todos que casam
  (sem `limit 1`), find sem projeção (retorna o documento inteiro), TLS via
  esquema `mongodb+srv://` (sem lookup DNS SRV), sem `OP_COMPRESSED`; document
  sequences (section kind 1) são puladas na leitura; uma conexão (com
  handshake `isMaster`) por chamada e payload inteiro em memória; banco por
  `MONGO_URL` (path) ou opção `banco:`.
- Kafka (`ler_kafka`/`escrever_kafka`/`fonte tipo: kafka`): wire protocol
  0.9-era — consumer groups com 1 membro por grupo por vez (o assignment
  "range" pega todas as partições, mas sem rebalanceamento real: dois
  consumidores no mesmo grupo não dividem as partições de forma coordenada),
  sem SASL (TLS via `{tls: verdadeiro}` nas opções), produce v1/fetch v1
  apenas, um broker líder por
  chamada e payload inteiro em memória.
- S3 (`ler_s3`/`escrever_s3`/`listar_s3`/`apagar_s3`): GET/PUT/LIST/DELETE
  com query string assinada (ListObjectsV2) — sem multipart/copy/presigned
  URLs e payload inteiro em memória; o parse do XML de listagem é por string
  simples (conteúdo de `<Key>`, entidades básicas); HTTP depende do binário
  `curl` e das credenciais via env (`AWS_ACCESS_KEY_ID`/`AWS_SECRET_ACCESS_KEY`).
  Funciona com S3-compatível (MinIO etc.) via `S3_ENDPOINT`.
- Bancos relacionais: somente consultas SELECT (sem INSERT/UPDATE via SQL,
  sem prepared statements); Postgres carrega `libpq.so.5` e SQLite
  `libsqlite3.so.0` via `dlopen` — precisam estar instalados no sistema.
- Redis: TLS via `rediss://` ou `{tls: verdadeiro}`, um comando por conexão,
  timeout fixo de 5s.
  AUTH via userinfo da URL (`redis://:senha@host`) ou opção `senha:`; SELECT
  via path numérico (`redis://host:6379/2`) ou opção `banco:`.
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
- `conv2d`/`norma_lote` → erro claro (ainda não existem).
- GPU: o backend CUDA (`TILT_GPU=auto`) só foi validado em hardware; aqui use
  `TILT_GPU=fake` para exercitar o caminho de dispatch.

## LLM / RAG

- Sem `TILT_LLM`, a chamada real depende do `curl` no `PATH`.
- `indice` roda com `armazenamento: "memoria"` (cosseno local),
  `"qdrant://host:porta/colecao"` (REST via curl) e `"pgvector://colecao"`
  (SQL sobre libpq, cosseno `<=>`; a tabela é criada automaticamente e
  `buscar` devolve `{ id, score }`, sem o texto).
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

## Plataforma

- `tilt compilar` gera x86-64; em ARM o teste `native` é pulado.
- Binário estático de libstdc++ só no Linux (no macOS usa a libc++ do sistema).
