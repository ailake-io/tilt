# 05 — LLMs e RAG

## `llm` — provedor

```tilt
llm gpt:
  provedor: "anthropic"             # anthropic | openai | local | vllm
  modelo: "claude-sonnet-5"
  temperatura: 0.2
  max_tokens: 1024
  chave: env "ANTHROPIC_API_KEY"    # segredo literal -> T020
  base_url: env "LLM_URL"           # para local/vllm (compatível OpenAI)
```

## Transporte

| `TILT_LLM` | Comportamento |
|---|---|
| `mock` | respostas e embeddings determinísticos, **offline** — usado nos testes |
| vazio | chamada real via `curl` (Anthropic `/v1/messages`, OpenAI `/chat/completions` e `/embeddings`) |

Em modo real o corpo vai num arquivo temporário e `curl --fail-with-body` faz o
POST; a resposta é parseada com o leitor JSON próprio. Falha de rede/chave →
erro de execução.

## `perguntar`

```tilt
fluxo resumir:
  entrada:
    documento: texto
  passos:
    - r = perguntar gpt:
        sistema: "Você resume em 3 frases."
        usuario: "Resuma:\n{{documento}}"
    - retornar r.texto
```

Retorna `{ texto, modelo }`. O primeiro argumento posicional é o `llm`
declarado; `sistema:` e `usuario:` vêm do bloco `:` (ou de `prompt:`).
`perguntar_em_fluxo` tem a mesma forma (streaming SSE no modo real).

## Saída estruturada

```tilt
tipo Ficha:
  nome: texto
  idade: inteiro
  nivel: "baixo" | "medio" | "alto"

fluxo extrair:
  entrada: { texto: texto }
  passos:
    - f = perguntar gpt, formato: Ficha:
        usuario: "Extraia dados de:\n{{texto}}"
    - retornar f              # mapa com os campos de Ficha
```

`formato: <Tipo>` devolve um `mapa` com os campos do `tipo`. No modo `mock` os
campos são sintetizados por tipo (texto → `"exemplo"`, inteiro → `0`, união →
primeiro literal); no modo real a resposta JSON é parseada e ausências caem no
padrão.

## `incorporar`

```tilt
- v = incorporar "text-embedding-3-small", "texto de exemplo"   # tensor[f32, D]
```

## `dividir_texto`

```tilt
- pedacos = dividir_texto documento, tamanho: 800, sobreposicao: 100   # lista de textos
```

## `indice` — RAG

```tilt
indice base:
  embeddings: "text-embedding-3-small"
  armazenamento: "memoria"          # qdrant:// / pgvector -> T900

pipeline indexar:
  passos:
    - base.inserir "a fatura sai no primeiro dia util do mes"
    - base.inserir docs           # lista/tabela: usa a coluna texto/conteudo/text/trecho, ou o valor
    - trechos = base.buscar "quando sai a fatura", top_k: 3
    - para cada h em trechos:
        imprimir h.id, h.score, h.texto
```

- `.inserir <lista | tabela | texto>` → número de itens adicionados. `id` vem
  da chave `id` da linha, se houver, senão é sequencial.
- `.buscar "consulta", top_k: N` → lista de `{ id, texto, score }` ordenada por
  similaridade de cosseno (embeddings determinísticos no modo `mock`).

Exemplo completo: [`../exemplos/rag_llm.tilt`](../exemplos/rag_llm.tilt).
