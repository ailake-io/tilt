# 05 — LLMs e RAG

## `llm` — provedor

```tilt run
llm gpt:
  provedor: "anthropic"             # anthropic | openai | local | vllm
  modelo: "claude-sonnet-5"
  temperatura: 0.2
  max_tokens: 1024
  chave: env "ANTHROPIC_API_KEY"    # segredo literal -> T020
  base_url: env "LLM_URL"           # para local/vllm (compatível OpenAI)
  tempo_limite: 60                  # segundos por tentativa (default 60)
  tentativas: 3                     # retry em transporte/429/5xx (default 3)
  teto_tokens: 0                    # 0 = sem teto; >0 barra antes de estourar
  reserva: [gpt_barato]             # fallback: outro 'llm' se este falhar

pipeline declara:
  passos:
    - imprimir "llm declarado"
```

## Robustez: retry, fallback, teto e tokens

- `tempo_limite:` aborta a tentativa (`curl --max-time`); `tentativas:`
  repete com backoff 1s → 2s → 4s… (teto 15s) em erro de transporte
  (inclui timeout), HTTP 429 e 5xx. Outros 4xx falham rápido, sem retry.
- `reserva: [b, c]` tenta outro `llm` declarado quando o primeiro esgota
  as tentativas (um nível, sem cadeia; repetido ou inexistente é erro
  claro antes da rede). Vale para `perguntar`, agentes e supervisor.
- `teto_tokens:` soma entrada+saída acumulados do `llm` no processo e
  falha **antes** da chamada que estouraria (erro `teto_tokens ...`).
- `perguntar` devolve `{texto, modelo, tokens: {entrada, saida}}` —
  `modelo` é o que respondeu (útil com `reserva:`), tokens vêm do `usage`
  da API (Anthropic `input/output_tokens`, OpenAI `prompt/completion_tokens`;
  no mock, heurística chars/4 por lado).

```tilt run
llm gpt:
  provedor: "anthropic"
  modelo: "claude-sonnet-5"
  chave: env "ANTHROPIC_API_KEY"
  tentativas: 3
  teto_tokens: 1000000

pipeline robusto:
  passos:
    # Com TILT_LLM=mock, tokens são chars/4 (determinístico) e o teto vale.
    - r = perguntar gpt, usuario: "oi"
    - imprimir r.texto
    - imprimir r.tokens.entrada, r.tokens.saida
    - imprimir r.modelo
```

Cobertura com HTTP de verdade em `tests/llm_retry_test.sh` (mock local com
429/500/timeout: retry, fallback, teto e `tempo_limite`).

## Transporte

| `TILT_LLM` | Comportamento |
|---|---|
| `mock` | respostas e embeddings determinísticos, **offline** — usado nos testes |
| vazio | chamada real via `curl` (Anthropic `/v1/messages`, OpenAI `/chat/completions` e `/embeddings`) |

Em modo real o corpo vai num arquivo temporário e `curl --fail-with-body` faz o
POST; a resposta é parseada com o leitor JSON próprio. Falha de rede/chave →
erro de execução.

## `perguntar`

```tilt run
llm gpt:
  provedor: "anthropic"
  modelo: "claude-sonnet-5"
  chave: env "ANTHROPIC_API_KEY"

pipeline resumir:
  passos:
    # Com TILT_LLM=mock, roda offline (resposta simulada determinística).
    - r = perguntar gpt:
        sistema: "Você resume em 3 frases."
        usuario: "Resuma:\ntilt é uma linguagem declarativa"
    - imprimir r.texto
```

Retorna `{ texto, modelo }`. O primeiro argumento posicional é o `llm`
declarado; `sistema:` e `usuario:` vêm do bloco `:` (ou de `prompt:`).
`perguntar_em_fluxo` tem a mesma forma (streaming SSE no modo real).

## Saída estruturada

```tilt run
llm gpt:
  provedor: "anthropic"
  modelo: "claude-sonnet-5"
  chave: env "ANTHROPIC_API_KEY"

tipo Ficha:
  nome: texto
  idade: inteiro
  nivel: "baixo" | "medio" | "alto"

pipeline extrair:
  passos:
    - f = perguntar gpt, formato: Ficha:
        usuario: "Extraia dados de:\nana, 30, alto"
    - imprimir f.nome, f.idade, f.nivel   # mapa com os campos de Ficha
```

`formato: <Tipo>` devolve um `mapa` com os campos do `tipo`. No modo `mock` os
campos são sintetizados por tipo (texto → `"exemplo"`, inteiro → `0`, união →
primeiro literal); no modo real a resposta JSON é parseada e ausências caem no
padrão.

## `incorporar`

```tilt run
pipeline vetores:
  passos:
    - v = incorporar "text-embedding-3-small", "texto de exemplo"   # tensor[f32, D]
    - imprimir v.forma   # [16] no mock determinístico
```

## `dividir_texto`

```tilt run
pipeline textos:
  passos:
    - doc = "primeira frase. segunda frase. terceira frase. quarta frase."
    - pedacos = dividir_texto doc, tamanho: 30, sobreposicao: 5   # lista de textos
    - imprimir tamanho pedacos
    - imprimir pedacos[0]
```

## `indice` — RAG

```tilt run
indice base:
  embeddings: "text-embedding-3-small"
  armazenamento: "memoria"          # ou qdrant://host:porta/colecao (REST via curl)
                                     # ou pgvector://colecao (Postgres + extensão pgvector)
                                     # ou weaviate://host:porta/classe (REST via curl)
                                     # ou pinecone://host-do-indice/namespace (HTTPS)
                                     # ou chroma://host[:porta]/colecao (HTTP, sem auth)

pipeline indexar:
  passos:
    # Com TILT_LLM=mock, embeddings determinísticos de 16 dimensões.
    - total = base.inserir([{ id: "a1", texto: "a fatura sai no primeiro dia util do mes" }])
    - imprimir total   # 1
    - base.inserir "o boleto vence dia dez"
    - trechos = base.buscar "quando sai a fatura", top_k: 3
    - para cada h em trechos:
        imprimir h.id, h.score
```

- `.inserir <lista | tabela | texto>` → número de itens adicionados. `id` vem
  da chave `id` da linha, se houver, senão é sequencial.
- `.buscar "consulta", top_k: N` → lista de `{ id, texto, score }` ordenada por
  similaridade de cosseno (embeddings determinísticos no modo `mock`).
- Com `armazenamento: "qdrant://host:porta/colecao"`, `.inserir`/`.buscar`
  delegam ao Qdrant (coleção criada automaticamente, distância Cosine; o id
  tilt vira UUID determinístico). Nesse modo `buscar` devolve `{ id, score }`
  — o texto fica no payload do ponto no Qdrant. Use parênteses para argumento
  lista: `base.inserir([...])`.
- Com `armazenamento: "pgvector://colecao"` + campo `url:` (connection string
  libpq, como em `fonte` postgres), delegam ao Postgres com a extensão
  pgvector: a tabela `<colecao>` é criada automaticamente na primeira escrita
  (`id TEXT PRIMARY KEY, texto TEXT, embedding vector(N)`), o upsert usa
  `ON CONFLICT` e a busca ordena por cosseno (`<=>`). `buscar` devolve
  `{ id, score }` (o texto fica na coluna `texto` da tabela). Exige a
  extensão `vector` instalada no banco (o Tilt tenta
  `CREATE EXTENSION IF NOT EXISTS vector`, que precisa de privilégio na
  primeira vez).
- Com `armazenamento: "weaviate://host:porta/classe"`, delegam ao Weaviate
  via REST (GraphQL `nearVector`, distância de cosseno; `score = 1 -
  distance`). A classe é criada automaticamente na primeira escrita
  (`vectorizer: "none"`, propriedade `texto`). Autenticação opcional: env
  `WEAVIATE_API_KEY` vira o header `Authorization: Bearer <chave>`; sem a
  env, a requisição é anônima. `buscar` devolve `{ id, score }` — o texto
  fica na propriedade `texto` do objeto. O nome da classe deve ser de
  GraphQL (`[A-Z][_a-zA-Z0-9]*`) e, no Weaviate real, o `id` deve ser UUID.
- Com `armazenamento: "pinecone://host-do-indice/namespace"` (ex.:
  `pinecone://meu-indice.svc.us-east1-gcp.pinecone.io/ns1`), delegam ao data
  plane do Pinecone, sempre em HTTPS. A env `PINECONE_API_KEY` é
  **obrigatória** (header `Api-Key`); sem ela, o erro é claro antes de tocar
  na rede. `inserir` faz upsert (`POST /vectors/upsert`, texto no
  `metadata.texto`); `buscar` usa `POST /query` e devolve `{ id, score }` —
  o score do Pinecone já é similaridade de cosseno (quanto maior, melhor).
  O índice deve **já existir** na conta (criar índice é control plane e está
  fora de escopo — `404`/`401` do servidor chegam como erro com a mensagem).
- Com `armazenamento: "chroma://host[:porta]/colecao"` (porta default
  **8000**), delegam ao Chroma via REST em HTTP puro, **sem auth** (padrão do
  Chroma open-source). A coleção é criada automaticamente na primeira
  escrita (`POST /api/v1/collections` com `get_or_create`); `buscar` usa
  `POST /api/v1/collections/{id}/query` e devolve `{ id, score }` — o Chroma
  devolve `distances` (`distance = 1 - cosseno`), então o score tilt é
  `1 - distance` (quanto maior, melhor, como nos demais). O texto fica em
  `metadatas[].texto` e `documents[]` do ponto.

Exemplo completo: [`../exemplos/rag_llm.tilt`](../exemplos/rag_llm.tilt).
