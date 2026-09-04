# 03 — Engenharia de dados

## `fonte` — conector de arquivo local

```tilt
fonte produtos:
  tipo: json                       # csv | json  (postgres/kafka/s3 -> T900)
  caminho: "dados/produtos.json"   # ou arquivo: / url:  ; aceita  env "VAR"
```

Num pipeline, `ler produtos` lê a fonte conforme `tipo:` e devolve uma
`tabela`. `file://` é removido do caminho. Conectores ainda não cobertos
(`kafka`, `s3`, `mongodb`, `iceberg`) levantam `T900` apontando o marco.

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
- a leitura aplica o log em ordem de versão (`add`/`remove`) e concatena os
  arquivos ativos, validando que o schema não diverge entre versões;
- interoperável com delta-rs: `DeltaTable(dir).to_pyarrow_table()` lê tabelas
  escritas pelo tilt, e o tilt lê tabelas delta-rs gravadas sem compressão,
  sem dictionary e com colunas obrigatórias;
- limitações: escrita sobrescreve a tabela (sem append/ACID concorrente), sem
  partições, sem checkpoints, sem transações concorrentes.

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
