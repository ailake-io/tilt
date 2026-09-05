# 03 — Engenharia de dados

## `fonte` — conector de arquivo local

```tilt
fonte produtos:
  tipo: json          # csv | json | parquet | delta | sqlite | postgres | kafka
  caminho: "dados/produtos.json"   # ou arquivo: / url:  ; aceita  env "VAR"
```

Num pipeline, `ler produtos` lê a fonte conforme `tipo:` e devolve uma
`tabela` (arquivos/SQL) ou `lista` de `texto` (kafka). `file://` é removido
do caminho. Conectores ainda não cobertos (`s3`, `qdrant`) levantam `T900`
apontando o marco. `fonte tipo: kafka` exige `topico:` (ver seção Kafka).

## `pipeline`

```tilt
pipeline etl:
  agenda: "0 * * * *"              # cron de 5 campos; ver --agendar abaixo
  ao_falhar: repetir 3             # reexecuta os passos ate 3x com escopo limpo
  passos:
    - bruto = ler_csv "clientes.csv"
    - limpo = bruto.filtrar linha.email contem "@"
    - agg = limpo.agrupar_por "dominio", { total: contar, receita: somar "valor" }
    - escrever_parquet agg, "saida.parquet"
```

`tilt executar` roda **todo** `pipeline` de topo, na ordem do arquivo. Sem
pipeline, roda uma `funcao principal` se existir.

`tilt executar --agendar` entra em **loop real de agenda**: pipelines sem
`agenda:` rodam uma vez na entrada; os demais disparam no próximo minuto que
casa com o cron (suporta `*`, `*/n`, `a-b`, `a-b/n` e listas `a,b`; 0 e 7 =
domingo). Antes de cada espera imprime `proxima execucao: AAAA-MM-DD HH:MM`.
Para testar sem esperar, use o relógio fake `TILT_AGORA=2026-01-05T02:50`
(hora local) com limite `TILT_AGENDAR_MAX=3` — o loop não dorme e avança o
tempo sozinho.

## Streaming com `janela:`

`janela:` é um campo de `pipeline` (ao lado de `agenda:`/`passos:`) que
controla **quando** os passos rodam dentro do loop de `--agendar`. Há três
formas:

```tilt
fonte eventos:
  tipo: csv
  caminho: "eventos.csv"

pipeline lotes:
  agenda: "*/1 * * * *"
  entrada: eventos        # fonte declarada com `fonte`
  janela: 100             # 1) contagem: passos rodam a cada 100 elementos novos
  passos:
    - imprimir tamanho(linhas), linhas[0].id

pipeline a_cada_5min:
  agenda: "*/1 * * * *"
  entrada: eventos
  janela: "5min"          # 2) tempo: passos rodam quando a janela decorre e há dados
  passos:
    - imprimir tamanho(linhas)

pipeline limitado:
  agenda: "*/1 * * * *"
  janela: "30s"           # 3) throttle: no máximo 1 execução por 30s
  passos:
    - imprimir "tick"

pipeline deslizante:      # janela de contagem com sobreposição entre lotes
  agenda: "*/1 * * * *"
  entrada: eventos
  janela: 3
  sobreposicao: 1         # mantém os 1 últimos elementos do lote anterior
  passos:
    - imprimir tamanho(linhas), linhas[0].id, linhas[-1].id
```

1. **Contagem** (`janela: N` inteiro, exige `entrada:`): a cada tick a fonte é
   relida por inteiro; os elementos além do último offset consumido entram num
   buffer. Quando o buffer acumula `N` elementos, os passos rodam **uma vez**
   com `linhas` = esses `N` elementos (na ordem da fonte), que saem do buffer.
   Sem elementos novos, os passos não rodam — contagem não depende do relógio.
2. **Tempo com entrada** (`janela: "30s"`, `"5min"`, `"1h"` + `entrada:`):
   novos elementos acumulam no buffer a cada tick; quando o relógio avançou a
   duração desde a última execução dos passos **e** há elementos pendentes,
   os passos rodam com `linhas` = tudo que acumulou, e o buffer zera.
3. **Throttle** (`janela: "<duracao>"` sem `entrada:`): limita a taxa do
   pipeline agendado — o primeiro tick executa e os seguintes só rodam quando
   a duração decorreu.

**Janela deslizante** (`sobreposicao: M`, irmão de `janela:`, default `0`):
com `janela: N` + `sobreposicao: M` (exige `N > M`), `linhas` continua tendo
os `N` elementos do lote, mas apenas `N - M` saem do buffer — os `M` últimos
repetem no início do próximo lote (janela deslizante clássica). Ex.: fonte
`1..5`, `janela: 3`, `sobreposicao: 1` → lotes `[1,2,3]` e `[3,4,5]`.

**Offset persistente**: em janela de contagem sobre fonte de arquivo
(`tipo: csv`/`json`), o offset fica gravado em `<caminho-da-fonte>.tilt-offset`
(JSON com um mapa por pipeline, p. ex. `{"contagens": 120}` — a chave é o
par pipeline + fonte, então pipelines diferentes sobre a mesma fonte não
interferem). O arquivo é gravado atomicamente (tmp + rename) sempre que o
offset avança, e lido na inicialização — reiniciar o processo continua de onde
parou, sem reprocessar elementos. Defina `TILT_JANELA_ESTADO=memoria` para
voltar ao comportamento antigo (só memória, sem arquivo — útil para testes e
pipelines efêmeros). Fonte Kafka com `grupo:` não usa arquivo: o checkpoint é
o commit de offsets do grupo no broker.

Fora de `--agendar`, um pipeline com `janela:` executa normalmente **uma vez**
(a janela "fecha" na primeira execução). Durações aceitas: `"Ns"`, `"Nmin"`,
`"Nh"` com `N` inteiro positivo.

## Leitura e escrita

| Builtin | Efeito |
|---|---|
| `ler_csv "caminho"` | → `tabela` (1ª linha = cabeçalho; célula vira inteiro/decimal/texto) |
| `ler_json "caminho"` | array de objetos → `tabela`; objeto → `mapa` |
| `ler_parquet "caminho"` | → `tabela` (ver Parquet abaixo) |
| `escrever_csv <tabela>, "caminho"` | grava CSV |
| `escrever_json <valor>, "caminho"` | grava JSON pretty (chaves em ordem de inserção) |
| `escrever_parquet <tabela>, "caminho"` | grava Parquet binário (ver Parquet abaixo) |
| `ler <fonte>` | lê a `fonte` declarada |
| `carregador "d.csv", alvo: "col"` | → `{ x: tensor[N,F], y: lista, atributos: lista }` |

## Parquet nativo

`ler_parquet`/`escrever_parquet` e `fonte tipo: parquet` usam o reader/writer
próprio do tilt (zero dependências), interoperável com pyarrow/parquet-cpp:

- tipos: `logico`→BOOLEAN, `inteiro`→INT64, `decimal`→DOUBLE, `texto`→BYTE_ARRAY;
- colunas **obrigatórias** (sem nulls), encoding **PLAIN**, **sem compressão**,
  um row group por arquivo;
- na escrita, a 1ª linha da tabela define o schema e todas as linhas precisam
  ter as mesmas colunas e tipos;
- na leitura, arquivos com compressão, campos opcionais (nulls) ou encoding
  diferente de PLAIN levantam erro claro (diz o que falta suportar).

Exemplo de interoperabilidade com Python:

```python
import pyarrow as pa, pyarrow.parquet as pq
schema = pa.schema([("nome", pa.string(), False), ("idade", pa.int64(), False)])
pq.write_table(tabela, "saida.parquet", compression="NONE", use_dictionary=False)
```

## Delta Lake mínimo

`escrever_delta`/`ler_delta` e `fonte tipo: delta` implementam o subconjunto de
1ª passada do protocolo Delta sobre diretório local:

- a escrita grava `<dir>/part-*.parquet` (mesmo perfil do Parquet acima) e o
  log `<dir>/_delta_log/00000000000000000000.json` com `protocol`, `metaData`
  (schemaString no formato JSON do Delta) e `add`;
- `anexar_delta tabela, "dir"` acrescenta linhas sem apagar o que já existe:
  valida que o schema (colunas em nome e ordem) é idêntico ao `metaData` da
  versão atual, grava um novo `part-*.parquet` e commita a próxima versão
  (`00000000000000000001.json`, ...) com `commitInfo` + `add`. O commit é
  atômico: o JSONL é gravado num temporário do mesmo diretório e publicado
  com `rename()` — crash antes do rename só deixa um parquet órfão, ignorado
  pela leitura. Diretório inexistente ou schema divergente → erro claro;
- a leitura aplica o log em ordem de versão (`add`/`remove`) e concatena os
  arquivos ativos, validando que o schema não diverge entre versões;
- interoperável com delta-rs: `DeltaTable(dir).to_pyarrow_table()` lê tabelas
  escritas pelo tilt, e o tilt lê tabelas delta-rs gravadas sem compressão,
  sem dictionary e com colunas obrigatórias;
- limitações: `escrever_delta` sobrescreve a tabela (recria a versão 0);
  `anexar_delta` pressupõe um único escritor (sem locks nem optimistic
  concurrency), sem partições, sem checkpoints, sem transações concorrentes.

## Iceberg (catálogo Hadoop)

`escrever_iceberg`/`anexar_iceberg`/`ler_iceberg` implementam o subconjunto de
1ª passada do Iceberg com catálogo tipo Hadoop (diretório local), Avro OCF
próprio (writer e reader, zero dependências) para manifest list + manifest e
Parquet nativo para os data files:

```tilt
- escrever_iceberg vendas, "tabela_iceberg"   # cria/sobrescreve (metadata v0)
- anexar_iceberg novas, "tabela_iceberg"      # append transacional (metadata v1, ...)
- tabela = ler_iceberg "tabela_iceberg"       # concatena os data files ativos
```

- layout: `<dir>/data/<uuid>.parquet`, `<dir>/metadata/<uuid>-m0.avro`
  (manifest), `<dir>/metadata/snap-<id>-0-<uuid>.avro` (manifest list) e
  `<dir>/metadata/v<N>-<uuid>.metadata.json` com `format-version: 2`, schema
  (tipos `texto`→string, `inteiro`→long, `decimal`→double, `logico`→boolean),
  `partition-specs` vazio e a lista de snapshots com `parent-snapshot-id`;
- `anexar_iceberg` valida que o schema é idêntico ao do metadata corrente,
  grava um novo data file e commita a próxima versão com um novo snapshot cujo
  manifest lista só o ADD do novo arquivo — os arquivos antigos permanecem
  visíveis pela cadeia de pais. O commit grava o metadata num temporário do
  mesmo diretório e publica com `rename()` (atômico no mesmo filesystem);
  diretório inexistente ou schema divergente → erro claro;
- a leitura resolve o snapshot atual percorrendo a cadeia de pais e coletando
  adds menos removes dos manifests (status 2 = DELETED), concatenando os data
  files com validação de schema entre arquivos;
- limitações: catálogo só Hadoop (diretório local, sem REST/JDBC), sem
  partições nem schema evolution, codec Avro "null" apenas, lê o que o tilt
  escreve (sem garantia de tabelas de outros escritores) e single-writer (sem
  locks nem optimistic concurrency).

## Bancos relacionais (SQLite e Postgres)

`fonte tipo: sqlite` e `fonte tipo: postgres` executam **consultas SELECT**
e devolvem `tabela`. Zero dependências de link: as bibliotecas são carregadas
em tempo de execução com `dlopen` (erro claro se ausentes).

```tilt
fonte clientes:
  tipo: postgres
  url: "host=localhost port=5432 dbname=app user=app"
  consulta: "select nome, idade from clientes where ativo"

fonte metricas:
  tipo: sqlite
  caminho: "metricas.db"
  consulta: "select dia, valor from vendas order by dia"

pipeline etl:
  passos:
    - novos = ler clientes
    - local = ler metricas
```

| Campo | Efeito |
|---|---|
| `caminho:` | SQLite: arquivo `.db` (deve existir) |
| `url:` | Postgres: connection string libpq |
| `consulta:` | SQL `SELECT` (INSERT/UPDATE/DDL → erro claro) |

Tipos: inteiro→`inteiro`, real/numeric→`decimal`, bool→`logico`,
texto→`texto`, NULL→`nulo`, BLOB SQLite→texto hex `0x...`.

## Redis (RESP nativo)

Sem dependências: cliente RESP próprio sobre socket TCP (sem hiredis).

```tilt
pipeline cache:
  passos:
    - escrever_redis "redis://localhost:6379", "perfil:1", { nome: "ana", idade: 30 }
    - perfil = ler_redis "redis://localhost:6379", "perfil:1"
    - imprimir perfil.nome
```

- `ler_redis url, chave`: GET; texto cru devolve `texto`, conteúdo que
  começa com `{`/`[` é parseado como JSON → `mapa`/`lista`; chave ausente →
  erro claro.
- `escrever_redis url, chave, valor`: SET; texto/numérico gravado como
  string, `mapa`/`lista` serializados como JSON compacto.
- limitações: sem TLS, sem AUTH, sem db index, um comando por conexão.

## S3 (AWS SigV4 próprio)

Sem dependências: SHA-256/HMAC implementados em C++ (FIPS 180-4 / RFC 2104)
e a assinatura AWS SigV4 calculada no próprio runtime; o HTTP sai pelo binário
`curl`, mesmo padrão do Qdrant/LLM.

```tilt
pipeline arquivos:
  passos:
    - escrever_s3 "s3://meu-bucket/relatorios/vendas.txt", "ola s3"
    - conteudo = ler_s3 "s3://meu-bucket/relatorios/vendas.txt"
    - imprimir conteudo
```

- `ler_s3 "s3://bucket/chave"`: GET do objeto, conteúdo devolvido como
  `texto` (a chave pode conter `/`).
- `escrever_s3 "s3://bucket/chave", valor`: PUT; `texto` vai bruto, demais
  valores são serializados com `json_dump`. Content-Type
  `application/octet-stream`.
- credenciais por variáveis de ambiente: `AWS_ACCESS_KEY_ID` e
  `AWS_SECRET_ACCESS_KEY` (obrigatórias, string vazia conta como ausente),
  `AWS_SESSION_TOKEN` (opcional), `AWS_REGION` (default `us-east-1`).
- `S3_ENDPOINT` (default `https://s3.<region>.amazonaws.com`): aponte para
  `http://host:porta` para S3-compatível (ex.: MinIO). O path do objeto é
  codificado por segmento e a query string fica vazia nesta 1ª passada.

## Kafka (wire protocol nativo)

Sem dependências: cliente do protocolo 0.9-era sobre socket TCP, com
CRC32-IEEE próprio para o message set — produce (api 0, v1), fetch (api 1,
v1), metadata (api 3, v0) e coordenação de consumer groups 0.9-era
(find_coordinator api 10, join_group api 11, heartbeat api 12,
leave_group api 13, sync_group api 14, offset_fetch api 9, offset_commit
api 8, v1). O broker vem da variável de ambiente `KAFKA_BOOTSTRAP`
(default `127.0.0.1:9092`) ou da opção `broker:`/`campo broker:`.

```tilt
pipeline eventos:
  passos:
    - escrever_kafka "pedidos", "msg-1"
    - escrever_kafka "pedidos", { id: 1, total: 99.9 }
    - msgs = ler_kafka "pedidos", { desde: "inicio", max: 10 }
    - imprimir tamanho msgs
    - novas = ler_kafka "pedidos", { grupo: "etl", max: 100 }
```

- `escrever_kafka topico, valor, {particao: N}`: produce com
  `required_acks=1`; `texto` vai bruto, demais valores são serializados com
  `json_dump`. `particao` é opcional (default 0). O cliente resolve o líder
  da partição via metadata e conecta nele.
- `ler_kafka topico, {desde:, max:, broker:}`: stateless — devolve `lista`
  de `texto` na ordem do log. `desde: "inicio"` (default) lê do earliest;
  `"fim"` lê do high watermark (só mensagens novas). `max` limita a
  quantidade (default 100).
- `ler_kafka topico, {grupo:, max:}`: consumer group com coordenação
  completa — o cliente resolve o coordenador (find_coordinator), entra no
  grupo (join_group com member_id vazio + sync_group com o assignor
  `"range"`), lê o offset commitado (offset_fetch), consome a partir dele
  (earliest se nunca commitado) e commita o offset seguinte ao último lido
  (offset_commit, metadata `"tilt"`) antes do leave_group. Com 1 membro no
  grupo, o assignment pega **todas** as partições do tópico, consumidas
  sequencialmente. Uma segunda chamada com o mesmo grupo só vê o que ainda
  não foi commitado — checkpoint natural entre execuções.
- `fonte tipo: kafka`: `topico:` é obrigatório; `grupo:` (checkpoint por
  commit de offset), `desde: "inicio"|"fim"`, `broker:` e `max:` são
  opcionais. `ler minha_fonte` devolve `lista` de `texto`. Em um `pipeline`
  com `janela:` e `agenda:`, a fonte **com** `grupo:` faz a janela acumular
  só mensagens novas a cada tick, porque o offset fica commitado no broker:

```tilt
fonte pedidos_kafka:
  tipo: kafka
  topico: "pedidos"
  grupo: "etl"          # offset commitado no broker = checkpoint da janela

pipeline agregacao:
  agenda: "* * * * *"
  entrada: pedidos_kafka
  janela: "1min"
  passos:
    - imprimir tamanho linhas, "pedidos no minuto"
```

- limitações: 1 membro por grupo por vez (sem rebalanceamento real entre
  consumidores), protocolo 0.9-era apenas, sem SASL/TLS (plain), um broker
  líder por chamada.

## MongoDB (BSON + OP_MSG nativos)

Sem dependências: BSON próprio (serializer + parser, tipos: double, string,
document, array, ObjectId, bool, datetime, null, int32, int64) e wire
protocol **OP_MSG** (opcode 2013) sobre socket TCP, com handshake
`{isMaster: 1}` no connect. O servidor vem da variável de ambiente
`MONGO_URL` (default `mongodb://127.0.0.1:27017`); o path opcional da URL é
o banco default (`mongodb://host:porta/banco`).

```tilt
pipeline pedidos:
  passos:
    - mongo_inserir "pedidos", {cliente: "ana", valor: 200}
    - achados = mongo_buscar "pedidos", {filtro: {cliente: "ana"}}
    - imprimir tamanho achados, achados[0].valor
```

- `mongo_inserir colecao, documento, {banco: "x"}`: o documento deve ser
  `mapa` (senão erro claro); gera `_id` ObjectId quando ausente. Mapeamento:
  `decimal`→double, `inteiro`→int64, `texto`→string, `logico`→bool,
  `nulo`→null, `mapa`→document, `lista`→array; `tensor`/`tabela` → erro
  claro. ObjectId vindo do servidor vira `texto` hex de 24 chars.
- `mongo_buscar colecao, {filtro:, max:, banco:}`: devolve `lista` de
  `mapas` (ordem do servidor). `filtro` é um `mapa` de **igualdade exata
  campo a campo** (top-level, combinado por E); omitido, retorna tudo.
  `max` limita a quantidade (default 100).
- banco efetivo: opção `banco:` > path do `MONGO_URL`; sem nenhum dos dois,
  `mongo_inserir`/`mongo_buscar` falham com erro acionável **antes** de
  tocar a rede.
- limitações da 1ª passada: sem update/delete/indexes/aggregate, filtro só
  por igualdade top-level, sem auth/TLS, sem `OP_COMPRESSED`; respostas com
  document sequences (section kind 1) são aceitas, mas `batchSize`/`max` de
  100 cabem na section kind 0 de qualquer forma.

## Índice vetorial no Qdrant

`indice` com `armazenamento: "qdrant://host:porta/colecao"` delega
`inserir`/`buscar` ao Qdrant via REST (curl, mesmo padrão do LLM). Os
embeddings continuam vindo de `embeddings:` (`TILT_LLM=mock` offline nos
testes). O id tilt (texto) é mapeado para UUID determinístico, pois o Qdrant
só aceita inteiro ou UUID. `inserir` cria a coleção automaticamente na
primeira chamada (distância Cosine).

```tilt
indice docs:
  embeddings: "meu-modelo"
  armazenamento: "qdrant://localhost:6333/docs"

pipeline rag:
  passos:
    - docs.inserir([{ id: "a1", texto: "gato doméstico" }])
    - achados = docs.buscar("gato", top_k: 3)
```

Nota: chame os métodos com parênteses quando o argumento é uma lista —
`docs.inserir([...])` — para não confundir o parser.

## Métodos de tabela

Operam sobre `tabela` e `lista` de mapas. `linha` é a variável implícita da linha atual.

| Método | Efeito |
|---|---|
| `.filtrar <cond>` | mantém as linhas onde `<cond>` (com `linha`) é verdadeira |
| `.derivar { col: <expr> }` | adiciona/atualiza colunas por linha |
| `.mapear { col: <expr> }` | idem `.derivar` |
| `.selecionar "a", "b"` | mantém só as colunas nomeadas |
| `.agrupar_por "col", { nome: <agg> }` | agrupa; `<agg>` ∈ `contar`, `somar "c"`, `media "c"`, `min "c"`, `max "c"` |
| `.ordenar_por "col", desc: verdadeiro` | ordena (numérico ou lexicográfico) |
| `.limite N` / `.primeiros N` | primeiras N linhas |
| `.distinto` / `.distinto "col"` | remove duplicatas |
| `.tamanho` | número de linhas |

```tilt
- vendas = ler_csv "vendas.csv"
- top = vendas.filtrar linha.valor >= 50
             .agrupar_por "regiao", { receita: somar "valor", n: contar }
             .ordenar_por "receita", desc: verdadeiro
             .limite 3
- para cada r em top:
    imprimir r.regiao, r.receita, r.n
```

## `verificar` — qualidade de dados

Passo dentro de `passos:` que valida uma tabela já no escopo:

```tilt
- verificar vendas:
    - nao_nulo: [regiao, valor]        # coluna ausente ou nula
    - unico: id                         # valor repetido
    - intervalo: linha.valor >= 0       # condição avaliada por linha
    ao_violar: abortar                  # abortar (T910) | avisar (segue)
```

Com `ao_violar: avisar` imprime `[aviso] verificar ...` e continua; com
`abortar` (padrão) lança `T910` com as primeiras violações como notas.

## Builtins de apoio

`tamanho` · `contar` · `somar`/`media`/`min`/`max` (sobre lista de números) ·
`intervalo n` / `intervalo a, b` (→ lista de inteiros) · `dividir "texto", "sep"`
(→ lista) · `dividir_texto "texto", tamanho: N, sobreposicao: M` (janela deslizante) ·
`imprimir` · `registrar` · `env "VAR"`.
