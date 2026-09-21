# 03 — Engenharia de dados

## `fonte` — conector de arquivo local

```tilt run
fonte produtos:
  tipo: json          # csv | json | parquet | delta | sqlite | postgres | duckdb | mysql | clickhouse | elasticsearch | opensearch | kafka
  caminho: "dados/produtos.json"   # ou arquivo: / url:  ; aceita  env "VAR"
```

Num pipeline, `ler produtos` lê a fonte conforme `tipo:` e devolve uma
`tabela` (arquivos/SQL) ou `lista` de `texto` (kafka). `file://` é removido
do caminho. S3 é usado pelos builtins `ler_s3`/`escrever_s3` e Qdrant pelo
bloco `indice`, mas eles não são fontes genéricas neste formato de `fonte`;
declarar esses valores como `fonte tipo:` continua levantando `T900`.
`fonte tipo: kafka` exige `topico:` (ver seção Kafka).

## `pipeline`

```tilt run
pipeline etl:
  agenda: "0 * * * *"              # cron de 5 campos; ver --agendar abaixo
  ao_falhar: repetir 3             # reexecuta os passos ate 3x com escopo limpo
  passos:
    - cru = [{ email: "ana@x.com", valor: 10 }, { email: "ruim", valor: 5 }]
    - escrever_csv cru, "clientes.csv"   # grava para o 'ler_csv' abaixo ler
    - bruto = ler_csv "clientes.csv"
    - limpo = bruto.filtrar linha.email contem "@"
    - agg = limpo.agrupar_por "email", { total: contar, receita: somar "valor" }
    - escrever_parquet agg, "saida.parquet"
    - imprimir tamanho agg   # 1
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
formas (o exemplo checa sem servidor; roda sob `tilt executar --agendar`):

```tilt check
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

**Estado persistente**: em janela de contagem ou de tempo sobre fonte de arquivo
(`tipo: csv`/`json`/`parquet`), o offset fica gravado em `<caminho-da-fonte>.tilt-offset`
(JSON com um mapa por pipeline; contagem usa o formato legado numero-puro, p.
ex. `{"contagens": 120}`; quando houver linhas acumuladas, o mapa guarda
`{offset, buffer}`; tempo/throttle guardam `{offset, last_run, buffer}` — a
chave é o par pipeline + fonte, então pipelines diferentes sobre a mesma fonte
não interferem). O arquivo é gravado atomicamente (tmp + rename) quando o
offset ou o buffer muda, e lido na inicialização — reiniciar o processo continua
de onde parou, sem reprocessar nem perder elementos já acumulados. Defina
`TILT_JANELA_ESTADO=memoria` para voltar ao comportamento antigo (só memória, sem
arquivo — útil para testes e pipelines efêmeros). Fonte Kafka com `grupo:` não usa arquivo: o checkpoint é
o commit de offsets do grupo no broker.

**Cursor incremental**: para fontes de arquivo, `desde: <coluna>` troca o
offset posicional por um watermark. A cada leitura, entram apenas as linhas
cujo valor da coluna é maior que o último valor consumido; cursores numéricos
e textuais são aceitos. O checkpoint usa o formato de mapa
`{"offset": 0, "cursor": valor}` (e mantém `last_run` quando aplicável),
permitindo retomar o pipeline sem reler valores já consumidos. O cursor deve
ser não nulo e comparável dentro da mesma fonte.

**Backfill**: um pipeline com cursor pode declarar
`backfill: { desde: valor, ate: valor }`. O intervalo é inclusivo e é lido uma
vez por processo para reprocessamento controlado; ele não avança o watermark
do fluxo incremental. Os limites precisam ser literais numéricos ou textuais
e compatíveis com a coluna indicada em `desde:`.

**Checkpoint distribuído (Marco 1)**: com `TILT_CHECKPOINT_DIR=<dir
compartilhado>` (NFS/EFS), o `.tilt-offset` mora nesse diretório (mesmo
basename) em vez do lado da fonte — réplicas diferentes passam a compartilhar
o offset. Com `TILT_CHECKPOINT_DIR=s3://<bucket>/<prefixo>`, o mapa vai como
um objeto `s3://<bucket>/<prefixo>/<basename>` (PUT/GET pelo cliente S3).
Com `TILT_CHECKPOINT_DIR=kafka:<topico>` (bootstrap via `KAFKA_BOOTSTRAP`), o
mapa inteiro vai como 1 record por save (leitura junta com last-wins, até 500
records; use tópico com compactação para históricos longos). S3 e Kafka
reaproveitam os conectores existentes. Combine com a eleição de líder abaixo
para ter escritor único (leituras concorrentes são seguras; escritas
concorrentes sem líder usam last-writer-wins).

**Eleição de líder (Fase 12-4)**: com `TILT_LEADER_LEASE=<path do lease>` (em
filesystem compartilhado) + `TILT_LEADER_TTL=<segundos, default 15>`, cada tick
do `--agendar` tenta o lease (criação `O_EXCL`; takeover quando expirado);
quem não é líder pula o tick com log `sem lideranca (...)`. Sem a env, o
comportamento é single-replica de sempre.

Fora de `--agendar`, um pipeline com `janela:` executa normalmente **uma vez**
(a janela "fecha" na primeira execução). Durações aceitas: `"Ns"`, `"Nmin"`,
`"Nh"` com `N` inteiro positivo.

## Operação: retry, callbacks, SLA, timeout, quarentena

Campos de `pipeline` para produção (combináveis):

```tilt run
importar io

pipeline instavel:
  # 1a tentativa: sem o marcador, falha e espera 1s; 2a: recupera.
  ao_falhar: repetir 2, espera: "1s", backoff: 2, jitter: "5s"
  passos:
    - se io.existe_arquivo "tentativa.txt":
        imprimir "recuperado na retentativa"
    senao:
        vazio = [{ n: 1 }]
        escrever_csv vazio, "tentativa.txt"
        x = ler_csv "sempre_ausente.csv"
```

- `ao_falhar: repetir N[, espera: "5s"][, backoff: F][, jitter: "2s"]`:
  reexecuta os passos do zero até N vezes com escopo limpo. Sem `espera:`,
  retenta de imediato; com `espera:`, dorme `espera × F^tentativa`. `jitter`
  adiciona aleatoriamente de zero até a duração informada (use `"0s"` para
  desativar); o atraso total permanece limitado a 5 minutos.
- `on_success:` e `on_failure:` executam blocos após sucesso ou depois que
  todas as tentativas falharem. Os callbacks recebem `pipeline`, `status`,
  `tentativas` e, no caminho de falha, `erro`; com `janela:` também recebem
  `linhas`.
- `sla: "30s"`: emite um alerta quando a duração total do pipeline excede o
  limite, sem transformar o alerta em falha. A medição usa relógio monotônico.
- Com `TILT_PIPELINE_LOG_JSON=1`, cada pipeline emite eventos JSON compactos
  com `evento`, `nome`, `status`, `tentativas` e `duracao_us`; falhas também
  incluem `erro`. O modo opt-in preserva a saída textual padrão.
- `tempo_limite: "30s"`: cada passo de topo tem esse teto; ao estourar, o
  passo falha (`passo K excedeu tempo_limite de 30s`) e o `ao_falhar` decide.
  A thread do passo segue destacada (o `Env` sobrevive), então use para
  passos que terminam sozinhos (I/O com timeout próprio) — laço infinito
  vaza thread até o fim do processo.
- `quarentena: "quarentena.jsonl"`: linhas do `para cada` que falham vão
  para o arquivo (JSONL `{"linha": ..., "erro": "..."}`) em vez de abortar;
  no fim imprime `quarentena: N linha(s) desviadas para ...`. Sem o campo,
  a primeira linha que falha aborta tudo (comportamento de sempre).

```tilt run
pipeline limpa:
  quarentena: "quarentena.jsonl"
  passos:
    - linhas = [{ v: 1 }, { v: 0 }, { v: 2 }]
    - para cada l em linhas:
        r = ler_csv "sempre_ausente.csv"   # falha nas 3: todas desviadas
    - imprimir "fim"   # o pipeline continua
```

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

### stdlib: `io`

Sobre esses builtins, a stdlib instalada com o tilt (`importar io`, resolução
no guia 01) adiciona helpers que falham como valor em vez de abortar o
pipeline:

| Função | Efeito |
|---|---|
| `io.juntar_caminhos(base, nome)` | junta com `/` → texto |
| `io.existe_arquivo(caminho)` | `logico` — verdadeiro se o arquivo abre para leitura como CSV/texto |
| `io.ler_json_seguro(caminho)` | valor JSON, ou `nulo` se faltar/inválido (útil para config opcional) |
| `io.salvar_json(caminho, valor)` | grava JSON, ordem `(caminho, valor)`; → `logico` |

Fora do escopo atual (sem builtin no runtime): ler arquivo bruto como texto,
tamanho em bytes e listar/remover arquivos.

### HTTP genérico (JSON)

O runtime expõe um cliente HTTP mínimo sobre `curl` (mesmo subprocesso dos
conectores — sem sockets próprios), com timeout padrão de 30s e erro claro em
falha de transporte, HTTP >= 400 (corpo do erro truncado na mensagem) ou JSON
inválido:

| Builtin | Efeito |
|---|---|
| `http_get_json "url", [cabecalhos:]` | → valor parseado do JSON da resposta |
| `http_post_json "url", valor, [cabecalhos:]` | envia `json_dump(valor)` com `Content-Type: application/json`; → valor parseado da resposta |

A URL deve começar com `http://` ou `https://`; `cabecalhos` é um mapa
`{ "Nome": "valor" }` de texto para texto (posicional ou nomeado). Erros
abortam como `T901` — envolva em `tentar`/`capturar` para tratá-los como
valor. Coberto pelo teste de integração `http_client`.

### stdlib: `rede`

Sobre esses builtins, a stdlib (`importar rede`) oferece as mesmas operações
com nomes em português:

| Função | Efeito |
|---|---|
| `rede.get_json(url, cabecalhos?)` | → valor JSON da resposta |
| `rede.post_json(url, corpo, cabecalhos?)` | envia `corpo` como JSON; → valor JSON da resposta |

De fora: outros verbos (PUT/PATCH/DELETE), corpo bruto (não-JSON) e
streaming — ver guia 12.

## Parquet nativo

`ler_parquet`/`escrever_parquet` e `fonte tipo: parquet` usam o reader/writer
próprio do tilt (zero dependências), interoperável com pyarrow/parquet-cpp e
com **Spark 3.5** (`spark.read.parquet`, `tests/spark_test.sh`):

- tipos: `logico`→BOOLEAN, `inteiro`→INT64, `decimal`→DOUBLE,
  `texto`→BYTE_ARRAY com anotação **UTF8** (lê como `string` no pyarrow);
  **listas de escalares** (`["a", "b"]`) viram campos REPEATED com anotação
  LIST (`list<string>`, `list<int64>`, `list<double>`, `list<bool>`);
  **mapas** viram grupos STRUCT recursivos (ver structs abaixo);
  estreitamento opt-in `tipos: { id: "int32", x: "float" }` grava
  INT32 (com anotação INTEGER, checagem de alcance) e FLOAT;
- **nulos**: coluna com `nulo` vira **OPTIONAL** (definition levels RLE,
  valores nulos omitidos das páginas); coluna sem nulos segue REQUIRED.
  Elemento Nulo dentro de lista vira element OPTIONAL; lista interna Nula
  (em `list<list<...>>`) e elemento struct Nulo (em lista de structs) viram
  grupos OPTIONAL. Uma coluna só de nulos (ou só de listas vazias) gera
  erro — o tipo não pode ser inferido;
- **compressão**: `escrever_parquet tabela, "saida.parquet", codec: "gzip"`
  (padrão), `codec: "snappy"` ou `codec: "zstd"` — o compressor snappy próprio é
  "literal-only" (emite um bloco snappy válido sem matching, sem redução de
  espaço), então qualquer leitor descomprime; a leitura descomprime snappy
  genérico (com matching), gzip/deflate (zlib via `dlopen("libz.so.1")`) e
  zstd (libzstd via `dlopen`, codec Parquet 6);
- **dictionary**: encoding DICTIONARY automático por coluna quando há
  repetição (dicionário em PLAIN + índices RLE; `dicionario: falso` volta ao
  PLAIN puro);
- **páginas**: DATA_PAGE v1 por padrão; `paginas: "v2"` grava DATA_PAGE_V2
  (definition/repetition levels fora da seção comprimida). A leitura aceita
  v1 e v2 de qualquer escritor;
- **Modular Encryption**: `escrever_parquet tabela, "seguro.parquet",
  chave: "segredo"` grava `PARE` com AES-GCM-256 (`AES_GCM_V1`), cifrando
  headers/payloads de páginas e o footer. A leitura exige a mesma chave em
  `TILT_PARQUET_KEY`; a chave não é persistida no arquivo e a autenticação
  rejeita arquivo adulterado ou segredo incorreto. O resolvedor AWS KMS ainda
  é uma etapa separada (o formato já preserva `FileCryptoMetaData` e
  `ColumnCryptoMetaData` padrão);
- escrita: encoding **PLAIN** ou **DICTIONARY** (acima), um row group por arquivo;
  a 1ª linha da tabela define o schema e todas as linhas precisam ter as
  mesmas colunas e tipos;
- leitura: **todos os row groups** (concatenados), campos REQUIRED, OPTIONAL
  e REPEATED, tipos físicos **INT32/INT64/FLOAT/DOUBLE/BOOLEAN/BYTE_ARRAY/
  FIXED_LEN_BYTE_ARRAY/INT96**, lógicos **UTF8/STRING/INTEGER/DATE/TIME/
  TIMESTAMP/DECIMAL** (data/hora/timestamp viram texto ISO, decimal vira
  decimal, dictionary pages PLAIN ou PLAIN_DICTIONARY), páginas **PLAIN** e
  **DICTIONARY** (`PLAIN_DICTIONARY`/`RLE_DICTIONARY`) e compressão
  **gzip/deflate**, **snappy** e **zstd**. Listas com 3+ níveis seguem com
  erro claro.

Exemplo de interoperabilidade com Python (arquivos de outras ferramentas —
dictionary, gzip/snappy, v2 e listas — são lidos diretamente):

```python
import pyarrow as pa, pyarrow.parquet as pq
schema = pa.schema([("nome", pa.string()), ("tags", pa.list_(pa.string()))])
pq.write_table(tabela, "saida.parquet", row_group_size=100_000,
               use_dictionary=True, compression="snappy",
               data_page_version="2.0")
```

## Delta Lake mínimo

`escrever_delta`/`ler_delta` e `fonte tipo: delta` implementam o subconjunto de
1ª passada do protocolo Delta sobre diretório local:

- a escrita grava `<dir>/part-*.parquet` (mesmo perfil do Parquet acima) e o
  log `<dir>/_delta_log/00000000000000000000.json` com `protocol`, `metaData`
  (schemaString no formato JSON do Delta) e `add`;
- `anexar_delta tabela, "dir"` acrescenta linhas sem apagar o que já existe:
  valida o schema **por nome** — toda coluna do `metaData` atual precisa
  existir na tabela anexada, colunas em comum com o mesmo tipo (a ordem é
  livre), e **colunas novas são permitidas** (evolução de schema): entram
  `nullable` no fim do `schemaString` e o commit carrega um `metaData` novo
  com o schema estendido. **Widening** sem perda também é evolução:
  `integer`→`long` e `float`→`double` (tabelas externas, ex. Spark) promovem o
  tipo no `schemaString` do commit, com field-id/posição estáveis. Arquivos
  antigos ficam sem a coluna nova e a leitura projeta `nulo` nas linhas deles
  (union-by-name). Remover coluna ou outra mudança de tipo → erro claro. Grava um novo `part-*.parquet` e commita
  a próxima versão (`00000000000000000001.json`, ...) com `commitInfo` (+
  `metaData`, se houve evolução) + `add`. O commit é
  atômico: o JSONL é gravado num temporário do mesmo diretório e publicado
  com `rename()` — crash antes do rename só deixa um parquet órfão, ignorado
  pela leitura. Diretório inexistente ou schema inválido → erro claro;
- a leitura aplica o log em ordem de versão (`add`/`remove`) e concatena os
  arquivos ativos, projetando cada arquivo no schema corrente por nome
  (coluna ausente no arquivo → nulo);
- **time travel**: `ler_delta "dir", versao: N` reconstrói exatamente o
  snapshot do commit `N`; checkpoints só são usados quando sua versão é menor
  ou igual ao snapshot solicitado;
- **Change Data Feed**: `ler_delta_mudancas "dir", de: N, ate: M` lê as ações
  `cdc` do intervalo quando presentes; sem `cdc`, deriva `insert`/`delete` de
  `add`/`remove`. Cada linha recebe `_change_type`, `_commit_version` e
  `_commit_timestamp`;
- **Deletion Vectors**: a leitura aplica `deletionVector` dos `add`/`remove`
  inline (`storageType: "i"`) e em arquivo (`"u"` relativo ou `"p"` absoluto),
  decodificando o RoaringBitmapArray portable de 64 bits (array, bitmap e run
  containers) e validando `sizeInBytes`, `cardinality` e CRC. A chave lógica
  do snapshot é `(path, deletionVector.uniqueId)`; checkpoints que não carregam
  o descriptor são ignorados para não reintroduzir linhas excluídas;
- **partições hive-style**: `escrever_delta tabela, "dir", particionar_por: "col"`
  (ou **partição composta** `particionar_por: ["estado", "ano"]`) grava os
  parquet em `<dir>/<c1>=<v1>/<c2>=<v2>/part-NNNNN.parquet`, **sem** as colunas
  de partição nos dados (padrão Delta — os valores vivem no diretório e no
  `partitionValues` de cada `add`; o `metaData` registra `partitionColumns` e o
  `schemaString` continua listando as colunas). Valor nulo em coluna de partição
  ou texto com `/` → erro claro (sem `__HIVE_DEFAULT_PARTITION__` nem
  escaping). `anexar_delta` herda a partição da tabela existente (chamar sem a
  opção ou com o mesmo valor, na mesma ordem); `particionar_por` explícito e
  divergente, ou opção em tabela não particionada → erro claro. Na leitura as
  colunas são reidratadas a partir de `partitionValues`, convertidas para o tipo
  declarado no schema (falha de conversão mantém texto). Tabelas particionadas
  por ferramentas externas (ex.: pyarrow/delta-rs) também são lidas;
- **pruning de partições**: `ler_delta "dir", onde: { estado: "sp", ano: 2024 }`
  compara predicados de igualdade contra as colunas de partição e **pula os
  arquivos que não podem conter linhas** (lê só os diretórios selecionados do
  log). Predicados em colunas comuns (não particionadas) viram filtro de linha
  aplicado após a leitura — o resultado é o mesmo, sem o custo de ler arquivo
  algum fora da partição. Sem match, retorna tabela vazia;
- interoperável com **Spark 3.5**: tabelas escritas pelo tilt são lidas por
  `spark.read.format("delta").load(dir)` (pacote `io.delta:delta-spark_2.12`,
  container `apache/spark:3.5.3` via `spark-submit`) — partição hive
  reidratada, append com evolução de schema (coluna nova visível com `null`
  nas linhas antigas). Coberto por `tests/spark_test.sh` (fase 12-1);
- interoperável com delta-rs: `DeltaTable(dir).to_pyarrow_table()` lê tabelas
  escritas pelo tilt, e o tilt lê tabelas delta-rs gravadas sem compressão,
  sem dictionary e com colunas obrigatórias;
- limitações: `escrever_delta` sobrescreve a tabela (recria a versão 0);
  `anexar_delta` pressupõe um único escritor (sem locks nem optimistic
  concurrency), sem transações concorrentes. **Checkpoint tilt-native (Fase
  12-5a)**: a cada 10 versões o append materializa
  `_delta_log/<v>.checkpoint.parquet` + `<v>.checkpoint.meta.json` e a leitura
  usa o checkpoint mais recente como base (só os JSONs maiores são
  repassados); arquivos ignorados por delta-rs/Spark. **Checkpoint padrão
  (Marco 1)**: `_last_checkpoint` + `<v>.checkpoint*.parquet` no schema
  oficial (add/remove/metaData, partitionValues MAP) é honrado como base —
  usa-se o de maior versão entre padrão e tilt-native.

### Manutenção de tabelas

`otimizar_delta "diretorio"` e `otimizar_iceberg "diretorio"` regravam os
arquivos ativos em conjuntos compactos, preservando o particionamento. Os
arquivos anteriores ficam órfãos até `vacuum_delta`/`vacuum_iceberg` removê-los.
Os aliases `optimize_delta` e `optimize_iceberg` também são aceitos. A
compactação é uma reescrita conservadora; no modo local, o metadata antigo é
substituído e o histórico removido deve ser mantido externamente se necessário.

Os writers também aceitam `z_order: ["coluna1", "coluna2"]`. A opção ordena as
linhas por uma chave Morton determinística antes da escrita, melhorando a
localidade de dados para leituras analíticas sem alterar o `particionar_por`.
São aceitas de uma a oito colunas escalares; nomes repetidos, colunas ausentes
ou linhas que não sejam mapas geram erro.

`vacuum_delta "diretorio"` e `vacuum_iceberg "diretorio"` removem Parquet
órfãos que não são referenciados por nenhum log Delta ou metadata/snapshot
Iceberg e devolvem a quantidade de arquivos removidos.

## Iceberg (catálogo Hadoop)

`escrever_iceberg`/`anexar_iceberg`/`ler_iceberg` implementam o subconjunto de
1ª passada do Iceberg com catálogo tipo Hadoop (diretório local), Avro OCF
próprio (writer e reader, zero dependências) para manifest list + manifest e
Parquet nativo para os data files. Os blocos OCF são gravados com codec
**deflate** (deflate RAW via zlib) por default; `ICEBERG_AVRO_CODEC`
(`null`/`deflate`/`snappy`) sobrescreve o codec de escrita — a leitura aceita
os três (snappy com trailer CRC32, conforme a spec Avro), independente da env:

```tilt run
pipeline iceberg_demo:
  passos:
    # Vendas inline; cada chamada abaixo roda de verdade no diretório local.
    - vendas = [
        { estado: "sp", ano: 2024, cidade: "santos", valor: 10 },
        { estado: "rj", ano: 2024, cidade: "niteroi", valor: 20 }
      ]
    - novas = [{ estado: "sp", ano: 2025, cidade: "campinas", valor: 30 }]
    - escrever_iceberg vendas, "tabela_iceberg"                    # cria/sobrescreve (metadata v0)
    - escrever_iceberg vendas, "tabela_iceberg", particionar_por: ["estado", "ano"]  # composta
    - anexar_iceberg novas, "tabela_iceberg"                       # append transacional (metadata v1, ...)
    - tabela = ler_iceberg "tabela_iceberg"                        # concatena os data files ativos
    - imprimir tamanho tabela   # 3
    - so_sp = ler_iceberg "tabela_iceberg", onde: { estado: "sp", ano: 2024 }  # pruning
    - imprimir tamanho so_sp    # 1
```

- layout: `<dir>/data/<uuid>.parquet` (tabela sem partição) ou
  `<dir>/data/<c1>=<v1>/<c2>=<v2>/00000-0-<uuid>.parquet` (naming iceberg:
  `<partition-path>/<file>.parquet`), `<dir>/metadata/<uuid>-m0.avro`
  (manifest), `<dir>/metadata/snap-<id>-0-<uuid>.avro` (manifest list) e
  `<dir>/metadata/v<N>.metadata.json` (nome canônico do HadoopCatalog, com
  `version-hint.txt` de apoio; o leitor também aceita o layout legado
  `v<N>-<uuid>.metadata.json`), com `format-version: 2`, `table-uuid` estável
  por tabela, `last-sequence-number`, `last-partition-id`, `sort-orders`,
  schema (tipos `texto`→string, `inteiro`→long, `decimal`→double,
  `logico`→boolean; as colunas de partição ficam `required: false`, como no
  Spark), snapshot e `partition-specs` com `default-spec-id: 0`; data files,
  manifests e manifest lists são referenciados por `file://` com **caminho
  absoluto** (legível de qualquer diretório de trabalho);
- partições: `particionar_por: "coluna"` (ou **partição
  composta** `particionar_por: ["estado", "ano"]`) aceita texto, inteiro,
  decimal ou lógico e cria um partition spec **identity** com um campo por
  coluna — `partition-spec` legado (lista de nomes) + `partition-specs` com
  `{field-id: 1000, 1001, ... (na ordem das colunas), source-id, transform:
  "identity", name}`; as colunas de partição **não vão no parquet** (os
  field-ids das demais colunas no arquivo seguem o id do schema Iceberg) e
  cada `data_file` do manifest ganha um record `partition` com um campo por
  coluna, no tipo da coluna (string/long/double/boolean). `ler_iceberg`
  resolve o spec do metadata e reidrata as colunas a partir dos manifests,
  convertendo pelo tipo do schema. Valor nulo em coluna de partição, texto
  com `/` e coluna repetida ou inexistente falham com erro claro (sem
  `__HIVE_DEFAULT_PARTITION__` nem escaping);
- **partição bucket (Fase 12-5a)**: `particionar_por: ["bucket[4](id)"]`
  (coluna inteira/texto/lógica) cria o campo `id_bucket_4` com transform
  `bucket[4]` (murmur3_x86_32 da spec, validado contra referência e
  pyiceberg); layout `id_bucket_4=<b>/00000-0-<uuid>.parquet`, record
  `partition` com o inteiro e coluna de origem **mantida no parquet**.
  `ler_iceberg ... onde: { id: 5 }` poda pelo hash (mais filtro residual
  exato contra colisões). Outros transforms (`truncate`, `year`, `month`,
  `day`, `hour`) são aceitos na leitura de tabelas externas sem poda por
  valor;
- **deletes (Fase 12-5a)**: `apagar_iceberg "dir", onde: { id: 2 }` escreve um
  position-delete file (`{file_path, pos}`) + snapshot `operation: "delete"`
  e devolve as linhas apagadas; `modo: "igualdade"` escreve equality deletes
  (linhas-chave). A leitura aplica ambos (posição por índice físico,
  igualdade por match em todas as colunas em comum, exigindo ao menos 1).
  Um delete file por partição atingida, com o record `partition`
  correspondente — pyiceberg aplica os position deletes do tilt;
- `anexar_iceberg` valida o schema **por nome** (todas as colunas do metadata
  corrente presentes na tabela anexada, tipos em comum iguais, ordem livre) e
  suporta **evolução de schema**: coluna nova entra `required: false` no fim
  do schema com **field-id novo** (`last-column-id` + 1); o metadata
  versionado ganha um `schema-id` novo mantendo o histórico de schemas (ids
  antigos estáveis) e os data files do append são gravados com esses
  field-ids. **Widening** sem perda (`int`→`long`, `float`→`double`, inclusive
  em subcolunas de struct) promove o tipo **mantendo o field-id** (promoção
  primitiva válida pela spec) com `schema-id` novo. Arquivos antigos ficam sem
  a coluna e a leitura projeta `nulo` nas linhas deles (union-by-name por
  field-id/nome — lido pelo pyiceberg).
  Remover coluna ou mudar o tipo de uma existente → erro claro. O append
  herda o partition spec da tabela (erro se `particionar_por` diverge ou se a
  tabela não é particionada), grava os novos data files e commita a próxima
  versão com um novo snapshot cujo manifest lista os arquivos já ativos como
  EXISTING (status 0) além do ADD (status 1) — como um append rápido de um
  writer real, para que um reader como pyiceberg resolva só pelos manifests
  do snapshot corrente. O commit grava o metadata num temporário do mesmo
  diretório e publica com `rename()` (atômico no mesmo filesystem);
  diretório inexistente ou schema inválido → erro claro;
- a leitura resolve o snapshot atual percorrendo a cadeia de pais e coletando
  adds menos removes dos manifests (status 2 = DELETED), concatenando os data
  files com validação de schema entre arquivos;
- **pruning de partições**: `ler_iceberg "dir", onde: { estado: "sp", ano: 2024 }`
  compara predicados de igualdade contra as colunas de partição e **pula os
  data files que não podem conter linhas** (record `partition` dos manifests).
  Predicados em colunas comuns (não particionadas) viram filtro de linha
  aplicado após a leitura — o resultado é o mesmo, sem o custo de ler arquivo
  algum fora da partição. Sem match, retorna tabela vazia;
- interop: metadata, manifest list, manifest e parquet carregam no
  **pyiceberg** (`StaticTable.from_metadata(...).scan().to_arrow()`) — field
  ids da spec v2 nos schemas Avro, `field.id` nos parquet e coluna de
  partição reidratada pelo reader de verdade;
- interop com **Spark 3.5**: `tests/spark_test.sh` sobe o Spark 3.5 real
  (container `apache/spark:3.5.3` + pacote
  `org.apache.iceberg:iceberg-spark-runtime-3.5_2.12`) e lê a tabela Hadoop
  local via `SparkCatalog` — metadata `v<N>.metadata.json`, partição identity
  reidratada, `file_path` absolutos e pruning real pelos summaries de
  partição (`contains_null`/`lower_bound`/`upper_bound`, record `r508`) do
  manifest list;
- limitações: sem o modo REST (abaixo) o catálogo é só Hadoop (diretório
  local, sem JDBC), lê o que o tilt escreve (sem garantia de tabelas de
  outros escritores além do subconjunto validado) e single-writer (sem locks
  nem optimistic concurrency). No modo REST, `apagar_iceberg` commita via
  `transactions` (add-snapshot + set-snapshot-ref) como o append.

### Iceberg REST catalog (opt-in, fase 29)

Com `ICEBERG_CATALOG=rest` + `ICEBERG_URI=http://host:porta` (HTTP via
`curl`, como S3/Qdrant/LLM), `escrever_iceberg`/`anexar_iceberg`/`ler_iceberg`
passam a operar via **Iceberg REST Open API** em vez do HadoopCatalog local.
Sem as env vars o comportamento é o local de sempre, byte a byte. O argumento
`dir` continua sendo a location da tabela — o tilt grava data files,
manifests e metadata localmente nesse diretório e envia a `location` como
`file://<caminho absoluto>` no `createTable`; o **nome da tabela no catálogo**
é o basename desse caminho, no namespace `default`:

```tilt check
# Mesmo código nos dois modos; com ICEBERG_CATALOG=rest + ICEBERG_URI o
# commit vai pelo REST (precisa do catálogo no ar para executar).
pipeline iceberg_rest_demo:
  passos:
    - vendas = [{ estado: "sp", valor: 10 }]
    - novas = [{ estado: "rj", valor: 20 }]
    - escrever_iceberg vendas, "tabela_iceberg"   # mesmo código dos dois modos
    - anexar_iceberg novas, "tabela_iceberg"
    - total = ler_iceberg "tabela_iceberg"
    - imprimir tamanho total
```

Subconjunto implementado (prefixo `v1`, namespace `default` — endpoints fora
dele não são inventados):

| Operação | Endpoint |
|---|---|
| loadTable | `GET /v1/namespaces/default/tables/<tabela>` → `config`, `metadata-location`, `metadata` |
| createTable | `POST /v1/namespaces/default/tables` com `{name, location, schema, partition-spec?, properties}` |
| commit | `POST /v1/namespaces/default/tables/<tabela>/transactions` com `requirements` + `updates` |

O tilt mantém o metadata em memória, então o commit só **envia updates**
referenciando as locations dos arquivos que já gravou localmente; o download
do metadata só acontece na leitura (`metadata-location` do loadTable —
`file://` lê direto do disco, `http(s)://` baixa via `curl`). Updates por
operação:

- **tabela nova** (loadTable 404 → createTable): requirement
  `assert-current-snapshot-id(-1)` + `upgrade-format-version(2)`,
  `set-location`, `set-properties`, `add-snapshot`, `set-snapshot-ref(main)`;
- **append**: `assert-current-snapshot-id(atual)` + `add-snapshot`,
  `set-snapshot-ref` (+ `add-schema` com `last-column-id` e
  `set-current-schema` quando o schema evoluiu — mesma regra do modo local);
- **sobrescrita de tabela existente**: `assert-current-snapshot-id(atual)` +
  `remove-snapshot-ref(main)`, `remove-snapshots(antigos)`, `add-schema`,
  `set-current-schema`, `add-snapshot`, `set-snapshot-ref` — o **partition
  spec é mantido**; `particionar_por` divergente em cima de tabela existente
  → erro claro (exclua a tabela no catálogo para recriar).

Erros claros: `ICEBERG_URI` ausente com `ICEBERG_CATALOG=rest`, respostas
não-2xx com o corpo do erro, JSON malformado e tabela inexistente no
`ler_iceberg` ("tabela `<nome>` não existe no catálogo REST"). Ainda é
single-writer (sem locks no catálogo) e 1ª passada: sem namespaces além de
`default`, sem paginação, sem OAuth e com location `file://` apenas. Fluxo
completo coberto por `tests/iceberg_rest_test.sh` (mock HTTP + validação do
metadata do "servidor" com pyiceberg `StaticTable.from_metadata`).

### `tilt servir-catalogo`: catálogo REST server (fase 30)

`tilt servir-catalogo <diretorio-raiz> [--porta N] [--prefixo P]` (porta
default **8191**) sobe um **servidor Iceberg REST Open API read-only** sobre as
tabelas Iceberg locais escritas pela tilt (formato Hadoop, subseção acima) —
uma engine como Spark SQL configura um `SparkCatalog` tipo `rest` com a URI
apontando para o tilt e lê as tabelas pelo nome, sem HadoopCatalog local:

```
tilt servir-catalogo /dados/iceberg --porta 8191
# tabelas servidas (2):
#   default.tabela_iceberg
#   default.vendas_part
# servir-catalogo: escutando http://127.0.0.1:8191/v1 (root: /dados/iceberg)
```

```python
spark = (SparkSession.builder
         .config("spark.sql.catalog.tiltcat", "org.apache.iceberg.spark.SparkCatalog")
         .config("spark.sql.catalog.tiltcat.type", "rest")
         .config("spark.sql.catalog.tiltcat.uri", "http://<host>:8191")
         .getOrCreate())
df = spark.read.table("tiltcat.default.tabela_iceberg")
```

Tabela Iceberg detectada = subdiretório direto do root que contém `metadata/`;
todas vivem no namespace `default`. Subconjunto v1 implementado (prefixo
configurável via `--prefixo`, default `/v1`):

| Rota | Resposta |
|---|---|
| `GET /v1/config` | `{"defaults":{},"overrides":{}}` |
| `GET /v1/namespaces` | `[["default"]]` |
| `GET /v1/namespaces/default` | namespace + properties vazios |
| `GET /v1/namespaces/default/tables` | identifiers de todas as tabelas |
| `GET /v1/namespaces/default/tables/<tabela>` | loadTable: `metadata-location` + `metadata` do `v<N>.metadata.json` mais recente (mesma regra do modo Hadoop: maior versão parseada do nome) + `config` |
| `GET/HEAD /v1/files/<rel-ao-root>` | bytes do arquivo (metadata.json, manifest `.avro`, data `.parquet`); a forma legada `?path=<abs>` também é aceita |

O loadTable **reescreve** as locations `file://<abs>` do metadata servido
(`snapshots[].manifest-list`) para URLs deste servidor
(`http://host:porta/v1/files/<caminho-relativo-ao-root>`) — usando o header
`Host` da requisição, para o cliente resolver o mesmo endereço que usou no
loadTable. A forma path-style (em vez de `?path=`) é de propósito: o `Path` do
Hadoop, usado pelo cliente Spark, re-encodea query strings. Assim o cliente
baixa metadata e manifest lists via HTTP (Iceberg aceita locations http(s) em
manifests/data); os arquivos servidos pelo `/v1/files` são **byte a byte** os
arquivos locais. O endpoint valida que o path resolvido (canonicalização real,
com symlinks) fica **dentro do diretório-raiz** — path traversal (`../`,
symlink para fora) recebe 403.

Ressalva engines Hadoop/Spark (1ª passada): o `fs.http` do Hadoop reporta
`getFileStatus().getLen() == -1` e o leitor Avro do Iceberg rejeita
`length < 4` sem ler o arquivo (`InvalidAvroMagicException`) — então, para o
Spark ler manifest lists pelo FileSystem, suba com **`--sem-reecrita-manifests`**
(mantém `file://` nas manifest-lists; o caller precisa acessar esses arquivos
locais, ex.: montando o diretório no mesmo path — como faz
`tests/spark_catalog_test.sh`). O `metadata-location` continua servido por
HTTP em todos os casos, e o restante do protocolo REST é idêntico. É **read-only** na 1ª passada: `createTable`/`commit`
(POST `…/transactions`)/HEAD de tabela respondem **501** com mensagem clara,
tabela ou rota inexistente respondem 404 (`NoSuchTableException` /
`NotFoundException`). Coberto por `tests/iceberg_catalog_test.sh` (cliente
urllib exercendo o subconjunto + traversal + read-only) e validado com
**Spark 3.5 real** via catálogo REST em `tests/spark_catalog_test.sh` (container
`apache/spark:3.5.3` + `iceberg-spark-runtime`, `--network host` e o dir montado
no mesmo path absoluto — manifests/data continuam `file://` absolutos).

## Limpeza e preparação de dados

Métodos de `tabela` que cobrem o dia a dia de limpeza sem escrever `derivar` linha a
linha. Todos devolvem uma tabela **nova** (a original não muda) e têm nome em inglês
equivalente (`drop_nulls`, `fill_nulls`, `rename`, `drop_columns`, `cast`, `deduplicate`,
`join`, `stack`, `describe`, `sample`, `value_counts`, `clean_text`; guia 18).

Uma célula é **nula** quando é `nulo`, a chave não existe ou é um texto vazio/só de
espaços (o que `ler_csv` devolve para célula vazia).

```tilt run
pipeline limpeza:
  passos:
    - bruto = [
        { id: 1, nome: "  Ana   Silva ", idade: "30", cidade: "sp", nascimento: "31/01/1994" },
        { id: 2, nome: "bruno", idade: "", cidade: "rj", nascimento: "1990-02-30" },
        { id: 2, nome: "bruno", idade: "", cidade: "rj", nascimento: "1990-02-30" },
        { id: 3, nome: "", idade: "1,5", cidade: nulo, nascimento: "2001/12/05" }
      ]
    # 1. conhecer os dados: tipo, nulos e distintos por coluna
    - para cada p em bruto.descrever:
        imprimir p.coluna, p.tipo, p.nulos, p.distintos
    # 2. limpar
    - a = bruto.deduplicar
    - b = a.limpar_texto "nome", caixa: "minusculas"
    - c = b.converter { idade: "inteiro", nascimento: "data" }
    - d = c.preencher_nulos { cidade: "desconhecida", idade: 0 }
    - e = d.remover_nulos "nome"
    - imprimir tamanho(e), e[0].nome, e[0].idade, e[0].nascimento, e[1].nascimento
    # 3. juntar com outra tabela e contar
    - ufs = [{ cidade: "sp", estado: "Sao Paulo" }, { cidade: "rj", estado: "Rio de Janeiro" }]
    - j = e.juntar ufs, por: "cidade", tipo: "esquerda"
    - imprimir j[0].estado, j[1].estado
    - cv = e.contar_valores "cidade"
    - imprimir cv[0].valor, cv[0].contagem
```

| Método | O que faz |
|---|---|
| `remover_nulos ["a", "b"]` | tira as linhas com nulo nessas colunas (todas, sem argumento) |
| `preencher_nulos { a: 0 }` / `preencher_nulos 0` | preenche nulos por coluna (cria a coluna se faltar) ou em todas |
| `renomear { antigo: "novo" }` | renomeia mantendo a ordem; coluna inexistente é erro |
| `remover_colunas "a", "b"` | tira colunas; nome inexistente é erro (pega typo) |
| `converter { c: "inteiro" }` | `inteiro`, `decimal` (aceita `1,5`), `texto`, `logico` (sim/não/true/false/1/0), `data` (→ `AAAA-MM-DD`). O que não converte vira `nulo`; fração em `inteiro` também (use `arredondar`) |
| `deduplicar ["a"]` | mantém a 1ª ocorrência por chave (linha inteira sem argumento); `distinto` é o mesmo |
| `juntar outra, por: "id", tipo: "esquerda"` | `interna` (padrão), `esquerda`, `direita`, `completa`; `por:` é nome, lista ou `{ esq: dir }`; colunas repetidas da direita ganham `_direita`; chave nula nunca casa |
| `empilhar(outra)` | concatena; o esquema vira a união das colunas |
| `descrever` | uma linha por coluna: `coluna, tipo, total, nulos, distintos, minimo, maximo, media` |
| `amostra 100, semente: 7` / `amostra 0.1` | amostra sem reposição, reproduzível, na ordem original |
| `contar_valores "col"` | `{ valor, contagem }` do mais ao menos frequente |
| `limpar_texto ["nome"], caixa: "minusculas"` | tira espaços das pontas e repetidos (e muda a caixa) |
| `ordenar_por "a", "b", desc: verdadeiro` | várias colunas, estável |

### Ler e gravar CSV de verdade

`ler_csv` aceita opções (todas opcionais; sem opção o comportamento é o de sempre):

```tilt skip
t = ler_csv "vendas.csv", separador: ";", pular: 2, nulos: ["NA", "-", ""],
      tipos: { valor: "decimal", data: "data" }
u = ler_csv "x.txt", separador: "auto"                      # detecta , ; tab |
v = ler_csv "sem_titulo.csv", sem_cabecalho: verdadeiro, colunas: ["id", "nome"]
```

- `separador:` um caractere, `"tab"` ou `"auto"`; `fonte tipo: csv` também aceita `separador:`.
- `pular: n` descarta as n primeiras linhas (títulos); `sem_cabecalho: verdadeiro` lê a
  primeira linha como dado (colunas `coluna1..N`, ou os nomes de `colunas:`).
- `nulos: [...]` lê esses textos como `nulo`; `tipos: { coluna: tipo }` converte na leitura
  (os mesmos tipos de `converter`, inclusive `"1,5"` como decimal e `31/01/2024` como data).
- `escrever_csv tabela, "x.csv", separador: ";"` coloca entre aspas o campo que tiver o
  separador, aspas ou quebra de linha (RFC 4180) e grava `nulo` como campo vazio.

### Datas e nulos

| Função | O que faz |
|---|---|
| `converter_data("31/01/2024")` | `2024-01-31` (aceita `AAAA-MM-DD`, `AAAA/MM/DD`, `DD/MM/AAAA` e hora opcional); `nulo` se inválida |
| `ano(d)`, `mes(d)`, `dia(d)` | partes de uma data ISO (`nulo` se inválida) |
| `adicionar_dias(d, n)` | soma (ou subtrai, com `n` negativo) dias |
| `dias_entre(a, b)` | dias de `a` até `b` (negativo se `b` < `a`) |
| `coalescer(a, b, ...)` | o primeiro valor que não é nulo nem texto vazio |


Métodos sem argumentos (`descrever`, `deduplicar`, `remover_nulos`, `limpar_texto`) podem
ser escritos sem parênteses; um argumento que seja lista literal exige parênteses
(`a.empilhar([...])`, pois `a.empilhar [...]` é lido como índice). Em 1 M de linhas,
`deduplicar`, `juntar` (600 mil linhas de resultado), `descrever`, `contar_valores` e
`converter` juntos levam ~2,4 s além da leitura. Para junções e agregações muito grandes,
`sql` com DuckDB (seção abaixo) é mais rápido.

## SQL sobre tabelas Tilt: `sql`

SQL é cidadão de primeira classe: `sql` roda uma consulta sobre tabelas que já estão
na memória do programa, sem servidor e sem `fonte`. O resultado é uma `tabela` normal
(`.filtrar`, `escrever_parquet`, outro `sql`...).

```tilt run
pipeline sql_local:
  passos:
    - vendas = [
        { regiao: "sul", valor: 30 },
        { regiao: "norte", valor: 120 },
        { regiao: "sul", valor: 5 }
      ]
    - clientes = [{ regiao: "sul", nome: "ana" }, { regiao: "norte", nome: "bia" }]
    # variaveis-tabela citadas no SQL entram sozinhas
    - resumo = sql "select regiao, sum(valor) as total from vendas group by regiao order by regiao"
    - imprimir resumo[0].regiao, resumo[0].total
    # join, parametros com ? e nome explicito
    - j = sql "select c.nome, sum(v.valor) as total from vendas v join clientes c on c.regiao = v.regiao group by c.nome order by c.nome"
    - imprimir j[0].nome, j[0].total
    - altos = sql "select * from vendas where valor >= ?", [30]
    - imprimir tamanho(altos)
    - n = sql "select count(*) as n from t", t: vendas
    - imprimir n[0].n
    # metodo: a propria tabela e `t`
    - m = vendas.sql "select max(valor) as maior from t"
    - imprimir m[0].maior
```

- **Tabelas**: variáveis-tabela citadas no SQL entram sozinhas; `nome: valor` registra
  explicitamente; `tabela.sql "... from t"` usa a própria tabela como `t`.
- **Parâmetros**: `sql "... where x >= ?", [30]` (o `?` é ligado por tipo, sem interpolar texto).
- **Colunas**: as chaves das linhas; o tipo (`INTEGER`, `REAL`, `TEXT`) vem dos valores.
  `logico` vira 0/1; lista/mapa viram texto JSON. Uma tabela vazia não tem colunas e dá erro.
- **Motor** (`motor: "sqlite" | "duckdb" | "auto"`, padrão `auto`): SQLite em memória
  para tabelas (sempre disponível com `libsqlite3`). Se a consulta lê arquivos
  (`from 'vendas.csv'`, `'x.parquet'`, `read_csv(...)`) o `auto` usa o **DuckDB**
  (`libduckdb.so` no `LD_LIBRARY_PATH`), que lê CSV/Parquet direto e é muito mais
  rápido em agregações grandes (1 M de linhas em ~0,2 s contra ~1,1 s do
  `ler_csv` + `agrupar_por`). Os dialetos diferem em detalhes (ex.: `7/2` é `3` no
  SQLite e `3.5` no DuckDB); fixe o `motor:` quando isso importar.
- Para bancos de verdade (Postgres, MySQL, SQLite em arquivo) use `fonte`, `consultar_sql`
  e `executar_sql` (seção abaixo).

## Bancos relacionais (SQLite, Postgres, DuckDB, MySQL/MariaDB e ClickHouse)

`fonte tipo: sqlite`, `fonte tipo: postgres`, `fonte tipo: duckdb`,
`fonte tipo: mysql` e `fonte tipo: clickhouse` executam **consultas SELECT**
e devolvem `tabela`. Zero dependências de link: SQLite/Postgres/DuckDB/MySQL
carregam as bibliotecas em tempo de execução com `dlopen` (erro claro se
ausentes); o ClickHouse fala HTTP nativo pelo cliente genérico do runtime
(subprocesso `curl`, mesmo padrão do s3/qdrant/iceberg REST).

Para comandos sem resultado — `INSERT`, `UPDATE`, `DELETE`, DDL — use o
builtin `executar_sql url, sql [, params]`, que aceita URL `postgres://` (ou
`postgresql://`), `sqlite://`, `duckdb://`, `mysql://` (ou `mariadb://`) e
`clickhouse://` (SQLite/DuckDB: o SQL roda direto no arquivo; o banco é criado
quando não existe). Retorna `nulo`; em caso de erro (ex.: violação de
constraint) lança a mensagem do servidor, capturável com `tentar`/`capturar`.
Um comando por chamada.

Valores nunca vão interpolados no SQL: passe `?` como placeholder e os valores
numa lista `[v1, v2, ...]` (inteiro, decimal, texto, logico ou nulo). O `?`
dentro de literais (`'...'`, `"..."`) e comentários (`--`, `/* */`) é
ignorado. Ligação por backend: postgres via `PQexecParams` (`?` vira `$N`),
sqlite por `sqlite3_bind_*`, duckdb por prepared statements (`duckdb_prepare`;
lib antiga sem esses símbolos falha com erro claro), mysql por prepared
server-side (`mysql_stmt_*`, tudo ligado como texto e coagido pelo servidor)
e clickhouse como query params `{pN:Tipo}` (nulo vira `NULL` inline).

Para passos atômicos use `transacao url, [{ sql:, params:? }]`: abre uma
conexão, roda `BEGIN`, executa os passos em ordem e faz `COMMIT`; qualquer
falha faz `ROLLBACK` e relança com o índice do passo (`passo 2: ...`).
ClickHouse não tem transações multi-comando via HTTP — `transacao` nele é erro
claro (execute os comandos com `executar_sql` um a um).

Para leituras parametrizadas use `consultar_sql url, sql [, params]`, a
contraparte de leitura: aceita as mesmas URLs, os mesmos placeholders `?` com
a mesma ligação por backend e devolve `tabela` (não precisa declarar `fonte`):

```tilt run
pipeline consulta:
  passos:
    # Banco local (sqlite cria o arquivo); mesma forma vale para
    # postgres://, mysql://, duckdb:// e clickhouse:// com servidor real.
    - url = "sqlite://banco.db"
    - executar_sql url, "create table clientes (nome text, idade integer, ativo integer)"
    - executar_sql url, "insert into clientes values (?, ?, ?)", ["ana", 30, verdadeiro]
    - executar_sql url, "insert into clientes values (?, ?, ?)", ["bob", 15, falso]
    - ativos = consultar_sql url, "select nome, idade from clientes where ativo = ? and idade >= ?", [verdadeiro, 18]
    - para cada c em ativos:
        imprimir c.nome, c.idade   # ana 30
```

Sem `params`, `consultar_sql url, sql` equivale à `consulta:` da `fonte`
(útil para SQL montado em runtime). Contagem divergente de `?` e SQL
não-SELECT (`INSERT` etc.) são erro claro.

```tilt check
# Fontes relacionais (precisam do servidor para executar; aqui checamos).
fonte clientes:
  tipo: postgres
  url: "host=localhost port=5432 dbname=app user=app"
  consulta: "select nome, idade from clientes where ativo"

fonte metricas:
  tipo: sqlite
  caminho: "metricas.db"
  consulta: "select dia, valor from vendas order by dia"

fonte analitico:
  tipo: duckdb
  arquivo: "analise.duckdb"   # ou ":memory:" (banco em memoria)
  consulta: "select regiao, sum(venda) as total from vendas group by regiao"

fonte pedidos:
  tipo: mysql
  url: "mysql://app:senha@localhost:3306/loja"   # mariadb:// tambem vale
  consulta: "select id, total from pedidos where status = 'pago'"

fonte eventos:
  tipo: clickhouse
  url: "clickhouse://default@localhost:8123/meubanco"
  consulta: "select dia, count() as total from eventos group by dia order by dia"

pipeline etl:
  passos:
    - executar_sql "postgres://localhost:5432/app", "insert into clientes (nome, idade) values ('ana', 30)"
    - executar_sql "duckdb://analise.duckdb", "create table vendas (regiao varchar, venda double)"
    - executar_sql "mysql://app:senha@localhost:3306/loja", "update pedidos set status = 'enviado' where id = 42"
    - executar_sql "clickhouse://default@localhost:8123/meubanco", "insert into eventos (dia) values ('2024-03-01')"
    - executar_sql "postgres://localhost:5432/app", "insert into clientes (nome, idade) values (?, ?)", ["o'brien", 30]
    - transacao "postgres://localhost:5432/app", [
        { sql: "insert into clientes (nome) values (?)", params: ["ana"] },
        { sql: "update clientes set idade = idade + ? where nome = ?", params: [1, "ana"] },
      ]
    - novos = ler clientes
    - local = ler metricas
    - por_regiao = ler analitico
    - pagos = ler pedidos
    - por_dia = ler eventos
```

| Campo | Efeito |
|---|---|
| `caminho:` | SQLite/DuckDB: arquivo do banco (deve existir; DuckDB aceita `:memory:`) |
| `url:` | Postgres: connection string libpq; MySQL/MariaDB: `mysql://usuario:senha@host:porta/banco` (porta default 3306; userinfo opcional); ClickHouse: `clickhouse://[usuario[:senha]@]host[:porta][/banco]` (HTTP; porta default 8123; sem userinfo usa `CLICKHOUSE_USER`/`CLICKHOUSE_PASSWORD` do ambiente; usuário default `default`) |
| `consulta:` | SQL `SELECT` (INSERT/UPDATE/DDL → erro claro; use `executar_sql`) |
| `pushdown:` | Opcional: `{colunas: ["..."], onde: {coluna: valor, ...}, limite: N}`; empurra projeção, filtros de igualdade e limite ao SQL parametrizado |

`pushdown:` também pode ser escrito como bloco indentado. `colunas` contém
nomes de coluna em texto; `onde` usa igualdade (`NULL` vira `IS NULL`) e
`limite` é um inteiro positivo. O Tilt envolve a `consulta:` original numa
subconsulta, liga os valores com os mesmos prepared statements de
`consultar_sql` e aplica a seleção no conector (SQLite, Postgres, DuckDB,
MySQL/MariaDB e ClickHouse). A consulta declarada não deve conter `?` quando
`onde:` for usado; para SQL já parametrizado, use `consultar_sql` diretamente.

Tipos: inteiro→`inteiro`, real/numeric→`decimal`, bool→`logico` (no DuckDB,
boolean→`inteiro` 0/1), texto→`texto`, NULL→`nulo`, BLOB SQLite→texto hex
`0x...` (no DuckDB, demais tipos como DATE/TIMESTAMP/UUID→`texto`). MySQL/
MariaDB: TINYINT/SMALLINT/INT/MEDIUMINT/BIGINT/YEAR→`inteiro` (TINYINT(1)
incluído — vira 0/1), DECIMAL/FLOAT/DOUBLE→`decimal` (DECIMAL chega como
texto e é convertido), demais tipos (VARCHAR, TEXT, DATE, DATETIME, JSON,
ENUM...)→`texto`. O conector MySQL carrega `libmariadb.so.3` ou
`libmysqlclient.so*` via `dlopen` (MariaDB e MySQL usam a mesma C API);
serve tanto contra MySQL quanto contra MariaDB.

### ClickHouse (HTTP nativo)

O ClickHouse não fala o protocolo wire dos relacionais acima: o conector usa o
**HTTP nativo** dele (`POST /?query=...`) pelo cliente genérico do runtime —
sem `dlopen`, só precisa do binário `curl`. A consulta da `fonte` é enviada em
`FORMAT JSONEachRow`, anexado automaticamente quando o SQL não traz um
`FORMAT` próprio; cada linha NDJSON vira um mapa da `tabela`. Tipos:
Int*/UInt*→`inteiro`, Float*/Decimal→`decimal`, String/FixedString/
Date/DateTime→`texto`, Nullable→`nulo` (JSON `null`, ou o marcador `ᴺᵁᴸᴸ`
quando um FORMAT TSV/CSV próprio for pedido), demais tipos (Array, Tuple,
Map...)→`texto` com a serialização JSON. Timeout de 60s (consultas
analíticas). Erros do servidor (HTTP ≥ 400, ex.: `Syntax error`) chegam como
exceção com o corpo do erro, capturável com `tentar`/`capturar`.

Gravação é `executar_sql` (INSERT/DDL/`ALTER`); o corpo da resposta é
ignorado. Não existe UPDATE transacional tradicional — mutações de linha são
`ALTER TABLE ... UPDATE` assíncronas —, então o fluxo típico é INSERT +
SELECT. Autenticação por userinfo da URL ou, sem ele, pelas env
`CLICKHOUSE_USER`/`CLICKHOUSE_PASSWORD`.

## Elasticsearch/OpenSearch (REST/JSON)

`fonte tipo: elasticsearch` (ou `opensearch`) executa `POST /<indice>/_search`
com o DSL de busca e devolve um **mapa** `{total, hits}` — `hits` é `tabela`,
com cada hit achatado um nível: `_id` mais os campos de `_source`. Agregações
(`aggregations` na resposta) vêm em `agregacoes`. Funciona tanto contra
Elasticsearch quanto contra OpenSearch (REST/JSON puro, sem dependências de
link — só o binário `curl`).

```tilt check
# Precisa do Elasticsearch/OpenSearch no ar para executar; aqui checamos.
fonte documentos:
  tipo: elasticsearch
  url: "elasticsearch://elastic:senha@localhost:9200/artigos"   # opensearch:// tambem vale
  consulta: """{"query": {"match_all": {}}}"""

pipeline busca:
  passos:
    - resultado = ler documentos
    - imprimir "total:", resultado.total
    - para cada hit em resultado.hits:
        imprimir hit._id, hit.titulo, hit.autor
    - por_autor = es_buscar "elasticsearch://localhost:9200/artigos", {size: 0, aggs: {por_autor: {terms: {field: "autor"}}}}
    - para cada b em por_autor.agregacoes.por_autor.buckets:
        imprimir b.key, b.doc_count
```

- URL: `elasticsearch://[usuario[:senha]@]host[:porta][/indice]` (porta
  default **9200**; `opensearch://` com mesmo formato). Sem usuário na URL a
  autenticação vem das env `ELASTIC_USER`/`ELASTIC_PASSWORD`; sem nenhum dos
  dois, as requisições saem **sem** header de autenticação. Com usuário, vai
  `Authorization: Basic` — mesmo com senha vazia.
- `consulta:` / `es_buscar url, dsl`: o DSL aceita **texto JSON** (string
  tripla `"""..."""` é a forma de embutir aspas) ou **mapa** tilt
  (`{query: {match_all: {}}}` é serializado automaticamente). Sem índice na
  URL a busca é em todos os índices (`POST /_search`).
- `es_executar url, metodo, caminho, [corpo]`: escape hatch genérica —
  `PUT`/`POST`/`GET`/`PATCH`/`DELETE`/`HEAD` em qualquer endpoint. O caminho
  é relativo ao índice da URL quando ele existe (`".../artigos"` +
  `"/_doc/1"` → `/artigos/_doc/1`), senão relativo à raiz. Corpo em texto é
  enviado direto; mapa/lista são serializados como JSON. A resposta vem
  parseada (`mapa`/`lista`/etc.); corpo vazio → `nulo` e corpo não-JSON
  (ex.: `/_cat/indices`) → `texto` cru.
- Erros do servidor (HTTP ≥ 400) lançam exceção com o motivo do
  `error.reason` do JSON de erro ("elasticsearch: no such index [x]"),
  capturável com `tentar`/`capturar`. Timeout de 30s.

## Redis (RESP nativo)

Sem dependências: cliente RESP próprio sobre socket TCP (sem hiredis).

```tilt check
# Precisa do Redis no ar (localhost:6379) para executar; aqui checamos.
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
- `redis_executar url, comando, [args...]`: envia um comando RESP arbitrário
  e devolve a resposta como valor tilt — simple string/bulk → `texto`,
  integer → `inteiro`, array → `lista` recursiva, nil → `nulo`, resposta de
  erro (`-ERR`) → exceção capturável (`redis: ...`). Args aceitam texto,
  lógico e numérico.
- `redis_lote url, [[comando, args...], ...]`: **pipeline** — envia todos os
  comandos numa única conexão (sem ler entre eles) e só então lê as N
  respostas na ordem, devolvendo a lista de valores. Limite de segurança de
  10 mil comandos.
- AUTH e seleção de banco: a URL aceita userinfo para a senha e path
  numérico para o db — `redis://:senha@host:6379/2` (AUTH `senha` + SELECT
  2). Também dá para passar como opções, que **vencem** a URL:
  `ler_redis url, "chave", {senha: "segredo", banco: 2}` /
  `escrever_redis url, "chave", valor, {banco: 2}`. Sem senha/banco, o
  comportamento é o de sempre (sem AUTH, db 0).
- **TLS**: use o esquema `rediss://` (`rediss://host:6379`) ou a opção
  `{tls: verdadeiro}` com URL comum. O OpenSSL é carregado em runtime via
  `dlopen` (`libssl.so.3`, fallback `libssl.so`) — zero dependência de link;
  o certificado do servidor é verificado contra o trust store do sistema e
  o hostname é conferido. Para certificado auto-assinado (ex.: em testes),
  defina `TILT_TLS_SKIP_VERIFY=1` para desligar a verificação. Sem
  client-cert/SASL nesta fase. O mesmo mecanismo cobre MongoDB e Kafka
  (ver abaixo). Vale também para `redis_executar`/`redis_lote` (via URL).
- limitações: `ler_redis`/`escrever_redis`/`redis_executar` abrem uma
  conexão por chamada; só `redis_lote` reaproveita a conexão (pipeline).

## S3 (AWS SigV4 próprio)

Sem dependências: SHA-256/HMAC implementados em C++ (FIPS 180-4 / RFC 2104)
e a assinatura AWS SigV4 calculada no próprio runtime; o HTTP sai pelo binário
`curl`, mesmo padrão do Qdrant/LLM.

```tilt check
# Precisa de credenciais AWS (ou MinIO via S3_ENDPOINT) para executar.
pipeline arquivos:
  passos:
    - escrever_s3 "s3://meu-bucket/relatorios/vendas.txt", "ola s3"
    - conteudo = ler_s3 "s3://meu-bucket/relatorios/vendas.txt"
    - imprimir conteudo
    - copiar_s3 "s3://meu-bucket/relatorios/vendas.txt", "s3://outro-bucket/vendas-copia.txt"
    - meta = cabecalho_s3 "s3://meu-bucket/relatorios/vendas.txt"
    - imprimir meta["content-length"]
    - apagar_s3 "s3://meu-bucket/relatorios/vendas.txt"
```

- `ler_s3 "s3://bucket/chave"`: GET do objeto, conteúdo devolvido como
  `texto` (a chave pode conter `/`).
- `escrever_s3 "s3://bucket/chave", valor`: PUT; `texto` vai bruto, demais
  valores são serializados com `json_dump`. Content-Type
  `application/octet-stream`.
- `listar_s3 "s3://bucket", {prefixo: "relatorios/", max: 100}`: listagem
  via ListObjectsV2 (`GET` no bucket com `list-type=2&prefix=...&max-keys=...`,
  chave e valor entram na assinatura SigV4); devolve `lista` de `texto` com
  as chaves ordenadas. Sem chaves no prefixo → lista vazia.
- `apagar_s3 "s3://bucket/chave"`: DELETE da chave; `204`/`200` ok, `404`
  levanta erro claro (`s3: objeto nao encontrado: <chave>`).
- `copiar_s3 "s3://bucket/a", "s3://outro-bucket/c"`: CopyObject — `PUT` no
  destino com o header `x-amz-copy-source` assinado; o bucket (e a chave) da
  origem podem diferir do destino.
- `cabecalho_s3 "s3://bucket/chave"`: HeadObject (`HEAD`, sem payload) →
  `mapa` com `content-length` (inteiro), `content-type`, `etag`,
  `last-modified` e os metadados `x-amz-meta-*` do objeto; `404` levanta
  erro claro (`s3: objeto nao encontrado: <chave>`).
- multipart upload, para objetos grandes:
  `uid = s3_iniciar_upload "s3://bucket/chave"` devolve o `uploadId`
  (`POST ?uploads`); `etag = s3_enviar_parte url, uid, 1, dados` envia uma
  parte numerada de `1` a `10000` (`PUT ?partNumber=N&uploadId=...`) e
  devolve o ETag da parte; `etag_final = s3_concluir_upload url, uid,
  [[1, e1], [2, e2]]` fecha o upload com o XML de CompleteMultipartUpload —
  a lista de partes deve ser não vazia, em ordem crescente e sem
  duplicatas; `s3_abortar_upload url, uid` descarta as partes enviadas
  (`DELETE ?uploadId=...`). Payload inteiro em memória.
- credenciais por variáveis de ambiente: `AWS_ACCESS_KEY_ID` e
  `AWS_SECRET_ACCESS_KEY` (obrigatórias, string vazia conta como ausente),
  `AWS_SESSION_TOKEN` (opcional), `AWS_REGION` (default `us-east-1`).
- `S3_ENDPOINT` (default `https://s3.<region>.amazonaws.com`): aponte para
  `http://host:porta` para S3-compatível (ex.: MinIO). O path do objeto é
  codificado por segmento e a query string (quando há, ex.: `listar_s3`) é
  codificada por chave/valor e incluída na assinatura.

## Kafka (wire protocol nativo)

Sem dependências: cliente do protocolo 0.9-era sobre socket TCP, com
CRC32-IEEE próprio para o message set — produce (api 0, v1), fetch (api 1,
v1), metadata (api 3, v0) e coordenação de consumer groups 0.9-era
(find_coordinator api 10, join_group api 11, heartbeat api 12,
leave_group api 13, sync_group api 14, offset_fetch api 9, offset_commit
api 8, v1). O broker vem da variável de ambiente `KAFKA_BOOTSTRAP`
(default `127.0.0.1:9092`) ou da opção `broker:`/`campo broker:`.

```tilt check
# Precisa do broker (KAFKA_BOOTSTRAP, default 127.0.0.1:9092) para executar.
pipeline eventos:
  passos:
    - escrever_kafka "pedidos", "msg-1"
    - escrever_kafka "pedidos", { id: 1, total: 99.9 }
    - msgs = ler_kafka "pedidos", { desde: "inicio", max: 10 }
    - imprimir tamanho msgs
    - novas = ler_kafka "pedidos", { grupo: "etl", max: 100 }
```

- `escrever_kafka topico, valor, {particao:, chave:, acks:, tentativas:, idempotente:}`:
  produce com `acks=-1` (all, default; `acks: 1` volta ao modo leader) e retry
  (default 3 tentativas em erros retriáveis 5/6/7 com refresh de metadata);
  `texto` vai bruto, demais valores são serializados com `json_dump`.
  `particao` é opcional (default 0); `chave:` (default vazio = NULL) envia a
  chave do record (compactação/dedup downstream). `idempotente:` (default
  verdadeiro) usa `InitProducerId` + Produce v3 com RecordBatch (magic 2,
  `producerId/epoch/sequence`, CRC32C) e sequência por partição: retry repete
  a mesma sequência e o broker deduplica (sem duplicatas em timeout/reenivo);
  `UnknownProducerId/OutOfOrderSequence` reinicializam o PID; sem suporte no
  broker (0.9-era) há fallback para MessageSet v1 com acks+retry. O cliente
  resolve o líder da partição via metadata e conecta nele. `{tls: verdadeiro}`
  liga TLS (todas as conexões da chamada: metadata, produce/fetch e coordenação
  de grupo; ver nota de TLS na seção Redis).
- Avro/Schema Registry (envelope Confluent): `avro_codificar valor, schema_json,
  id_esquema` devolve `magic-byte 0 + schema-id big-endian + payload Avro`; o
  inverso é `avro_decodificar payload, schema_json`, que devolve
  `{ id_esquema:, valor: }`. Para buscar ou registrar schemas no registry use
  `avro_schema url, id_esquema` e `avro_registrar url, subject, schema_json`.
  `escrever_kafka` aceita `{ formato: "avro", schema:, id_esquema: }` e
  `ler_kafka` aceita `{ formato: "avro", schema: }`, devolvendo os valores
  decodificados. O codec suporta os tipos Avro usados pelo runtime (primitivos,
  records, arrays, maps e uniões anuláveis); compatibilidade/evolução de schema
  continua sendo responsabilidade do Schema Registry.
- `transacao_kafka id, registros, {broker:, acks:, tentativas:, tls:}`: inicia uma
  transação Kafka real com `FindCoordinator`, `InitProducerId`,
  `AddPartitionsToTxn`, Produce v3 com o bit transacional e `EndTxn`. Cada item
  de `registros` é `{topico:, valor:, particao:, chave:?}`; todas as partições
  são adicionadas antes da primeira publicação e o commit confirma o lote
  atomicamente. Em falha, o cliente envia `EndTxn` abortado; `tentativas:`
  repete produces retriáveis com a mesma sequência. O broker deve suportar
  transações Kafka (não há fallback silencioso para brokers 0.9-era).
- `ler_kafka topico, {desde:, max:, broker:, tls:}`: stateless — devolve `lista`
  de `texto` na ordem do log. `desde: "inicio"` (default) lê do earliest;
  `"fim"` lê do high watermark (só mensagens novas). `max` limita a
  quantidade (default 100).
- `ler_kafka topico, {grupo:, max:}`: consumer group com coordenação completa
  e rebalanceamento — o cliente resolve o coordenador (find_coordinator),
  entra no grupo (join_group com o assignor `"roundrobin"`), o **líder**
  calcula o assignment das partições a partir da lista de membros e o
  distribui (sync_group), um heartbeat periódico (a cada 3s, em thread) mantém
  a membresia, e o consumo lê o offset commitado (offset_fetch) a partir dele
  (earliest se nunca commitado), commitando o offset seguinte ao último lido
  **por partição** (offset_commit, metadata `"tilt"`) antes do leave_group
  limpo. Dois ou mais consumidores no mesmo grupo dividem as partições do
  tópico entre si; se um rebalance ocorrer durante o consumo
  (RebalanceInProgress/IllegalGeneration no fetch, heartbeat ou commit), o
  cliente re-entra no grupo (novo Join/Sync) e retoma do offset commitado, sem
  duplicar mensagens. Uma segunda chamada com o mesmo grupo só vê o que ainda
  não foi commitado — checkpoint natural entre execuções.
- `fonte tipo: kafka`: `topico:` é obrigatório; `grupo:` (checkpoint por
  commit de offset), `desde: "inicio"|"fim"`, `broker:` e `max:` são
  opcionais. `ler minha_fonte` devolve `lista` de `texto`. Em um `pipeline`
  com `janela:` e `agenda:`, a fonte **com** `grupo:` faz a janela acumular
  só mensagens novas a cada tick, porque o offset fica commitado no broker:

O exemplo completo [`exemplos/kafka_streaming.tilt`](../exemplos/kafka_streaming.tilt)
processa um lote de eventos com `janela:` e imprime cada evento recebido. Ele é
exercitado pelo teste de integração `tests/kafka_test.sh` usando o broker mock.

```tilt check
# Janela sobre Kafka com grupo (checkpoint no broker); roda com --agendar.
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

- limitações: protocolo 0.9-era apenas (assignor único `"roundrobin"`,
  sem `consumer.intervals` negociados), sem SASL, um broker líder por
  chamada. A detecção de entrada/saída de membros no meio do consumo só é
  revalidada no próximo heartbeat (a cada 3s) ou na próxima chamada — não há
  push imediato de revogação para um fetch já em andamento.

## MongoDB (BSON + OP_MSG nativos)

Sem dependências: BSON próprio (serializer + parser, tipos: double, string,
document, array, ObjectId, bool, datetime, null, int32, int64) e wire
protocol **OP_MSG** (opcode 2013) sobre socket TCP, com handshake
`{isMaster: 1}` no connect. O servidor vem da variável de ambiente
`MONGO_URL` (default `mongodb://127.0.0.1:27017`); o path opcional da URL é
o banco default (`mongodb://host:porta/banco`). O esquema
`mongodb+srv://host:porta/banco` liga **TLS** (mesma camada do Redis; sem
lookup DNS SRV nesta fase — o host é usado como em `mongodb://`).

```tilt check
# Precisa do MongoDB (MONGO_URL, default 127.0.0.1:27017) para executar.
pipeline pedidos:
  passos:
    - mongo_inserir "pedidos", {cliente: "ana", valor: 200}
    - achados = mongo_buscar "pedidos", {filtro: {cliente: "ana"}}
    - imprimir tamanho achados, achados[0].valor
    - mods = mongo_atualizar "pedidos", {cliente: "ana"}, {"$set": {valor: 999}, "$inc": {acessos: 1}}
    - imprimir mods                              # nModified (inteiro)
    - resumo = mongo_buscar "pedidos", {somente: ["cliente", "valor"]}
    - imprimir resumo                            # só os campos pedidos
    - deletados = mongo_deletar "pedidos", {cliente: "bob"}
    - imprimir deletados                         # n deletados (inteiro)
    - nome = mongo_criar_indice "pedidos", {campos: ["cliente"]}
    - imprimir nome                              # "cliente_1"
    - totais = mongo_agregar "pedidos", [{"$group": {"_id": "$cliente", "total": {"$sum": "$valor"}}}]
    - imprimir totais                            # lista de mapas (firstBatch)
```

- `mongo_inserir colecao, documento, {banco: "x"}`: o documento deve ser
  `mapa` (senão erro claro); gera `_id` ObjectId quando ausente. Mapeamento:
  `decimal`→double, `inteiro`→int64, `texto`→string, `logico`→bool,
  `nulo`→null, `mapa`→document, `lista`→array; `tensor`/`tabela` → erro
  claro. ObjectId vindo do servidor vira `texto` hex de 24 chars.
- `mongo_buscar colecao, {filtro:, max:, somente:, lote:, banco:}`: devolve
  `lista` de `mapas` (ordem do servidor). `filtro` é um `mapa` de
  **igualdade exata campo a campo** (top-level, combinado por E); omitido,
  retorna tudo. `max` limita a quantidade total (default 100). `somente`
  (lista de textos) é uma **projeção whitelist**: só os campos listados
  voltam (`{campo: 1, ...}` no comando find; o `_id` continua vindo, como
  no `mongo` de verdade). `lote` (inteiro > 0) é o `batchSize` pedido ao
  servidor: se o cursor vier aberto (`cursor.id != 0`), o conector itera
  `getMore` automaticamente acumulando `nextBatch` até o cursor fechar
  (limite de segurança de 10000 getMore por busca).
- `mongo_atualizar colecao, filtro, mudancas, {banco:, multi:}`: devolve
  `nModified` (inteiro). `mudancas` é um mapa de operadores: `$set:
  {campo: valor, ...}` e `$inc: {campo: delta, ...}` são suportados e
  podem ser **combinados no mesmo update** (`{$set: {...}, $inc: {...}}`);
  outro operador → erro claro. `filtro` é o
  mesmo de `mongo_buscar`. `multi` (lógico, default `falso`): `falso`
  atualiza só o primeiro documento que casa; `verdadeiro`, todos.
- `mongo_deletar colecao, filtro, {banco:}`: remove **todos** os documentos
  que casam com `filtro` (igualdade top-level; 1ª passada sem "só o
  primeiro") e devolve `n` deletados (inteiro).
- `mongo_criar_indice colecao, {campos: ["a", "b"], banco:}`: cria índice
  ascendente (`{a: 1, b: 1}`) e devolve o `name` gerado dos campos
  (texto, ex.: `"a_1_b_1"`).
- `mongo_agregar colecao, [etapas], {banco:}`: roda o **pipeline de
  aggregation** e devolve o `cursor.firstBatch` como `lista` de `mapas`
  (mesma conversão BSON→tilt de `mongo_buscar`). As etapas são mapas tilt
  traduzidos **1:1** para BSON — chaves com `$` precisam ser **texto entre
  aspas** (`{"$group": ...}`), pois `$` cru não é válido na sintaxe tilt.
  Uso típico: `$match` (igualdade e `$eq`/`$gt`/`$gte`/
  `$lt`/`$lte`/`$ne`/`$in`, combinados por `$and`/`$or`), `$project`
  (`1`/`verdadeiro` inclui, `0`/`falso` exclui), `$group` (`_id: "$campo"`
  com acumuladores `$sum` (`1` ou `"$campo"`, também usado para `$count`),
  `$avg`, `$min`, `$max`), `$sort` (`1`/`-1`), `$limit` e `$skip`. A
  semântica não é validada localmente: etapa inválida vira erro claro com a
  mensagem do servidor. Sem `getMore` nesta fase — só o primeiro batch é
  lido (`cursor.id` ignorado); use `$limit`/`$skip` para resultados maiores.
- banco efetivo: opção `banco:` > path do `MONGO_URL`; sem nenhum dos dois,
  os builtins falham com erro acionável **antes** de tocar a rede.
- limitações da 1ª passada: sem `getMore` no `mongo_agregar` (só
  `firstBatch`), sem `$unset`/demais operadores de update (`$set` e `$inc`
  rodam), projeção só whitelist (sem exclusões tipo `{campo: 0}`/`_id:
  falso`), sem índices de texto/TTL, filtro de `mongo_buscar` só por igualdade
  top-level, `mongo_deletar` sempre remove todos que casam (sem `limit 1`),
  sem auth/TLS, sem `OP_COMPRESSED`; respostas com document sequences
  (section kind 1) são aceitas, mas `batchSize`/`max` de 100 cabem na section
  kind 0 de qualquer forma.

## Spark via Livy

`fonte tipo: spark` e os builtins `spark_sql`/`spark_executar` falam REST/JSON
com um servidor [Apache Livy](https://livy.apache.org/) (`http://host:8998`) —
a ponte zero-link para **Spark SQL** e para executar código num cluster Spark
sem linkar nada: o cliente HTTP genérico do runtime (subprocesso `curl`, mesmo
padrão do ClickHouse/Elasticsearch/S3) implementa o subset do Livy: criação de
sessão (`POST /sessions` com `{kind, conf}`), listagem (`GET /sessions`),
statements (`POST /sessions/{id}/statements`) e polling de estado
(`GET /sessions/{id}/statements/{st}` até `available`/`error`, timeout ~120s).

```tilt check
# Precisa do Livy (http://host:8998) com Spark para executar.
fonte vendas:
  tipo: spark
  url: "http://localhost:8998"
  consulta: "select id, nome from vendas order by id"

fonte agregada:
  tipo: spark
  url: "http://localhost:8998"
  lingua: "pyspark"            # default "scala" (sessão Livy kind "spark")
  conf: {"spark.master": "yarn", "spark.jars.packages": "org.example:lib:1.0"}

pipeline etl:
  passos:
    - linhas = ler vendas                       # SELECT -> tabela
    - total = spark_sql "http://localhost:8998", "select count(*) as total from vendas"
    - saida = spark_executar "http://localhost:8998", "spark.range(100).filter(\"id % 2 = 0\").count()"
```

| Campo/opção | Efeito |
|---|---|
| `url:` | endereço do servidor Livy (`http://host:8998`) |
| `consulta:` | SQL `SELECT` (Spark SQL) — o cliente envolve em `spark.sql("""...""")` e parseia o `toJSON` da resposta em `tabela` |
| `lingua:` | `"scala"` (default; sessão Livy `kind: "spark"`) ou `"pyspark"` (sessão `kind: "pyspark"`; wrapper em Python) |
| `conf:` | mapa de conf da **sessão** (ex.: `spark.master`, `spark.jars.packages`); só vale na criação |

- `spark_sql url, sql, [lingua:, conf:]` devolve `tabela` (lista de objetos
  JSON → linhas de mapa; números, textos e nulos chegam tipados). O SQL roda
  como `spark.sql("""<sql>""").toJSON.collectAsList().toString()` em Scala
  (ou o equivalente `json.dumps` sobre `toJSON().collect()` em PySpark).
- `spark_executar url, codigo, [lingua:, conf:]` envia o código **verbatim**
  (Scala ou Python conforme `lingua:`) e devolve o texto do output do
  statement (`texto`) — útil para pipelines `RDD`/`Dataset`, `CREATE TABLE`,
  `INSERT`, etc.
- **Sessões não são fechadas**: criar uma sessão faz o driver Spark subir do
  zero, então o cliente lista `GET /sessions` e **reutiliza a primeira sessão
  idle com o `kind` correspondente**; só cria nova quando não há nenhuma. A
  sessão fica no pool do Livy (que derruba por inatividade conforme o conf do
  servidor, ex.: `livy.server.session.timeout`). O `conf:` só é enviado no
  `POST /sessions` de criação.
- Erros: statement em `error` vêm como exceção `livy: <ename>: <evalue>`
  (mensagem do Spark), capturável com `tentar`/`capturar`; sessão que morre ao
  iniciar e timeout de polling (~120s) também são reportados.
- Autenticação: Livy sem auth (rede interna) nesta fase; sem Kerberos/HTTPS
  próprio do cliente.
- Coberto por `tests/livy_test.sh` (mock REST em python3 com o subset acima —
  criação/listagem de sessões, statements com polling e output simulando
  `toJSON`, reuso da sessão idle entre execuções).

## Índice vetorial no Qdrant

`indice` com `armazenamento: "qdrant://host:porta/colecao"` delega
`inserir`/`buscar` ao Qdrant via REST (curl, mesmo padrão do LLM). Os
embeddings continuam vindo de `embeddings:` (`TILT_LLM=mock` offline nos
testes). O id tilt (texto) é mapeado para UUID determinístico, pois o Qdrant
só aceita inteiro ou UUID. `inserir` cria a coleção automaticamente na
primeira chamada (distância Cosine).

```tilt check
# Precisa do Qdrant (localhost:6333) para executar; aqui checamos.
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

## Índice vetorial no Weaviate

`indice` com `armazenamento: "weaviate://host:porta/classe"` delega
`inserir`/`buscar` ao Weaviate via REST (curl, mesmo padrão do Qdrant). Os
embeddings continuam vindo de `embeddings:` (`TILT_LLM=mock` offline nos
testes). `inserir` cria a classe automaticamente na primeira chamada
(`vectorizer: "none"`, propriedade `texto`) e faz upsert do objeto
(`PUT /v1/objects/<classe>/<id>`); `buscar` usa GraphQL `nearVector`
(distância de cosseno, `score = 1 - distance`) e devolve `{ id, score }`.
Autenticação opcional via env `WEAVIATE_API_KEY`
(`Authorization: Bearer <chave>`); sem a env, anônimo. Coberto por
`tests/weaviate_test.sh` (mock REST em python3 + embeddings em modo `mock`).

```tilt check
# Precisa do Weaviate (localhost:8080) para executar; aqui checamos.
indice docs:
  embeddings: "meu-modelo"
  armazenamento: "weaviate://localhost:8080/Documentos"

pipeline rag:
  passos:
    - docs.inserir([{ id: "a1", texto: "gato doméstico" }])
    - achados = docs.buscar("gato", top_k: 3)
```

## Índice vetorial no Pinecone

`indice` com `armazenamento: "pinecone://host-do-indice/namespace"` delega
`inserir`/`buscar` ao data plane do Pinecone via REST, sempre em HTTPS (curl,
mesmo padrão do Qdrant/Weaviate). Os embeddings continuam vindo de
`embeddings:` (`TILT_LLM=mock` offline nos testes). A env `PINECONE_API_KEY`
é obrigatória (header `Api-Key`); ausente, o erro é claro antes da rede.
`inserir` faz upsert (`POST /vectors/upsert`,
`{namespace, vectors: [{id, values, metadata: {texto}}]}`); `buscar` usa
`POST /query` (`{namespace, vector, topK}`) e devolve `{ id, score }`; o score
do Pinecone já é similaridade de cosseno. Depois do upsert, `inserir` valida o
namespace com `describe_index_stats`, e `buscar` faz essa validação antes da query.
O índice deve **já existir** na conta: criar índice é control plane e fica fora de escopo.
`tests/pinecone_test.sh` (mock REST sobre TLS com cert auto-assinado +
embeddings em modo `mock`).

```tilt check
# Precisa do Pinecone (índice existente + PINECONE_API_KEY) para executar.
indice docs:
  embeddings: "meu-modelo"
  armazenamento: "pinecone://meu-indice-abc.svc.us-east1-gcp.pinecone.io/ns1"

pipeline rag:
  passos:
    - docs.inserir([{ id: "a1", texto: "gato doméstico" }])
    - achados = docs.buscar("gato", top_k: 3)
```

## Índice vetorial no Chroma

`indice` com `armazenamento: "chroma://host[:porta]/colecao"` (porta default
8000) delega `inserir`/`buscar` ao Chroma via REST em HTTP puro (curl, mesmo
padrão do Qdrant/Weaviate), **sem auth** (padrão do Chroma open-source). Os
embeddings continuam vindo de `embeddings:` (`TILT_LLM=mock` offline nos
testes). A coleção é get-or-create (`POST /api/v1/collections`,
`{name, get_or_create}`) e o `id` devolvido endereça o add (`POST .../add`,
`{ids, embeddings, metadatas, documents}`) e a query (`POST .../query`,
`{query_embeddings, n_results}`). A query devolve `distances` — com
`hnsw:space cosine`, `distance = 1 - cosseno` — e o score tilt é
`1 - distance` (quanto maior, melhor, consistente com os demais backends).
`buscar` devolve `{ id, score }` (o texto fica em `metadatas[].texto` /
`documents[]`). Coberto por `tests/chroma_test.sh` (mock REST em python3 +
embeddings em modo `mock`).

```tilt check
# Precisa do Chroma (localhost:8000) para executar; aqui checamos.
indice docs:
  embeddings: "meu-modelo"
  armazenamento: "chroma://localhost:8000/docs"

pipeline rag:
  passos:
    - docs.inserir([{ id: "a1", texto: "gato doméstico" }])
    - achados = docs.buscar("gato", top_k: 3)
```

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

```tilt run
pipeline metodos:
  passos:
    # Métodos encadeiam por atribuição (um por passo).
    - vendas = [
        { regiao: "sul", valor: 30 },
        { regiao: "sul", valor: 80 },
        { regiao: "norte", valor: 120 }
      ]
    - top = vendas.filtrar linha.valor >= 50
    - agg = top.agrupar_por "regiao", { receita: somar "valor", n: contar }
    - ord = agg.ordenar_por "receita", desc: verdadeiro
    - top3 = ord.limite 3
    - para cada r em top3:
        imprimir r.regiao, r.receita, r.n
```

## `verificar` — qualidade de dados

Passo dentro de `passos:` que valida uma tabela já no escopo:

```tilt run
pipeline checa_vendas:
  passos:
    - vendas = [
        { id: 1, regiao: "sul", valor: 30 },
        { id: 2, regiao: "norte", valor: 120 }
      ]
    - verificar vendas:
        - nao_nulo: [regiao, valor]        # coluna ausente ou nula
        - unico: id                         # valor repetido
        - intervalo: linha.valor >= 0       # condição avaliada por linha
        ao_violar: abortar                  # abortar (T910) | avisar (segue)
    - imprimir "vendas ok"
```

Com `ao_violar: avisar` imprime `[aviso] verificar ...` e continua; com
`abortar` (padrão) lança `T910` com as primeiras violações como notas.

## Builtins de apoio

`tamanho` · `contar` · `somar`/`media`/`min`/`max` (sobre lista de números) ·
`intervalo n` / `intervalo a, b` (→ lista de inteiros) · `dividir "texto", "sep"`
(→ lista) · `dividir_texto "texto", tamanho: N, sobreposicao: M` (janela deslizante) ·
`imprimir` · `registrar` · `env "VAR"`.
