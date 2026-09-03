# 06 — Agentes

## `ferramenta`

```tilt
ferramenta busca_documentos:
  descricao: "Busca trechos relevantes na base."
  entrada:
    termo: texto
    limite: inteiro = 5
  executar:
    vetor = incorporar "text-embedding-3-small", termo
    retornar base.buscar vetor, top_k: limite
```

- `entrada:` define os parâmetros da ferramenta.
- `executar:` é um bloco de instruções; o `retornar` produz o resultado.
- Chamada direta: `busca_documentos(termo: "x")` ou `busca_documentos.executar { termo: "x" }`.

## `agente`

```tilt
agente AssistenteTecnico:
  llm: gpt
  papel: "Especialista em análise preditiva e dados estruturados."
  ferramentas:
    - busca_documentos
  memoria: conversa                 # nenhuma | conversa
  max_passos: 8
```

`A.responder "mensagem"` (ou `A.perguntar "..."`):

1. roda cada ferramenta listada uma vez, com entradas best-effort a partir da
   mensagem (campos `texto` recebem a mensagem; os demais, o padrão do tipo);
2. junta as observações;
3. pede a resposta ao `llm` com `papel` + observações como sistema.

Retorna:

```tilt
{ texto: "...",
  rastro: [ { passo: 1, ferramenta: "busca_documentos", observacao: "..." } ] }
```

`memoria: conversa` mantém um histórico por agente, prefixado no prompt das
chamadas seguintes.

> O planner iterativo (o LLM escolhendo a próxima ação a cada passo) chega numa
> próxima passada; hoje o laço no modo `mock` é determinístico.

## `equipe`

```tilt
equipe PesquisaEEscrita:
  agentes:
    - pesquisador: AssistenteTecnico
    - escritor: RedatorTecnico
  estrategia: sequencial            # sequencial | paralelo  (supervisor -> M9.2)
```

`E.executar "mensagem"` (ou `E.responder`):

- `sequencial` — a saída de um agente é a entrada do próximo; retorna a última.
- `paralelo` — todos recebem a mesma mensagem; retorna `rotulo: texto` concatenado.

Retorna `{ texto, rastro: [ { agente, texto } ] }`.

## Exemplo

[`../exemplos/agente.tilt`](../exemplos/agente.tilt) — ferramenta + RAG + agente.
[`../exemplos/copiloto.tilt`](../exemplos/copiloto.tilt) — agente que revisa
código Tilt com o builtin `checar_tilt` (ver [guia 10](guia-10-ia-editores.md)).

```bash
TILT_LLM=mock tilt executar exemplos/agente.tilt
```
