# 06 — Agentes

## `ferramenta`

```tilt check
# 'executar' sem '-' (corpo direto). 'base' é o índice declarado no guia 05.
indice base:
  embeddings: "text-embedding-3-small"
  armazenamento: "memoria"
  dimensao: 16
  metrica: cosseno

ferramenta busca_documentos:
  descricao: "Busca trechos relevantes na base."
  entrada:
    termo: texto
    limite: inteiro
  executar:
    vetor = incorporar "text-embedding-3-small", termo
    retornar base.buscar vetor, top_k: limite
```

- `entrada:` define os parâmetros da ferramenta.
- `executar:` é um bloco de instruções; o `retornar` produz o resultado.
- Chamada direta: `busca_documentos(termo: "x")` ou `busca_documentos.executar { termo: "x" }`.

## `agente`

```tilt run
llm gpt:
  provedor: "anthropic"
  modelo: "claude-sonnet-5"
  chave: env "ANTHROPIC_API_KEY"

ferramenta eco:
  descricao: "Repete o texto de volta."
  entrada:
    texto: texto
  executar:
    retornar texto

agente AssistenteTecnico:
  llm: gpt
  papel: "Especialista em análise preditiva e dados estruturados."
  ferramentas:
    - eco
  memoria: conversa                 # nenhuma | conversa | vetorial
  max_passos: 8

pipeline pergunta:
  passos:
    # Com TILT_LLM=mock, o planner chama cada ferramenta uma vez, em ordem.
    - r = AssistenteTecnico.responder "resuma tilt"
    - imprimir r.texto
```

`A.responder "mensagem"` (ou `A.perguntar "..."`) usa um **planner
iterativo**: a cada passo o LLM escolhe a próxima ação, até `max_passos`:

1. o interpretador envia o `papel`, a lista de ferramentas (nome, descrição,
   parâmetros) e o histórico de observações;
2. o LLM responde **exatamente uma linha**:
   - `chamar <nome> {<json de argumentos>}` — executa a ferramenta (argumentos
     ausentes são preenchidos best-effort: campos `texto` recebem o pedido,
     os demais o padrão do tipo); a observação entra no histórico;
   - `responder: <texto>` — encerra o loop com a resposta final;
3. resposta fora do protocolo é tratada como resposta final (fallback
   tolerante para LLMs reais);
4. sem `llm:` declarado, cada ferramenta roda uma vez com entradas
   best-effort e a resposta é local (`[sem llm] ...`).

Retorna (ilustração da forma; ver exemplo executável no fim do guia):

```tilt skip
{ texto: "...",
  rastro: [ { passo: 1, ferramenta: "busca_documentos",
              argumentos: {...}, observacao: "..." } ] }
```

`memoria: conversa` mantém um histórico por agente, prefixado no prompt das
chamadas seguintes. No modo `TILT_LLM=mock` o planner é determinístico: cada
ferramenta é chamada uma vez, na ordem declarada, e depois o mock responde.

## `equipe`

```tilt run
llm gpt:
  provedor: "anthropic"
  modelo: "claude-sonnet-5"
  chave: env "ANTHROPIC_API_KEY"

ferramenta eco:
  descricao: "Repete o texto de volta."
  entrada:
    texto: texto
  executar:
    retornar texto

agente Pesquisador:
  llm: gpt
  papel: "Pesquisa fontes."
  ferramentas: [eco]
  memoria: conversa
  max_passos: 2

agente Redator:
  llm: gpt
  papel: "Escreve o relatório."
  ferramentas: [eco]
  memoria: conversa
  max_passos: 2

equipe PesquisaEEscrita:
  agentes:
    - pesquisador: Pesquisador
    - escritor: Redator
  estrategia: supervisor       # sequencial | paralelo | supervisor
  supervisor: gpt
  objetivo: "Produzir relatório técnico com fontes citadas."
  max_passos: 6

pipeline relatorio:
  passos:
    - r = PesquisaEEscrita.responder "relatório sobre tilt"
    - imprimir r.texto
```

`E.executar "mensagem"` (ou `E.responder`):

- `sequencial` — a saída de um agente é a entrada do próximo; retorna a última.
- `paralelo` — todos recebem a mesma mensagem; retorna `rotulo: texto` concatenado.
- `supervisor` — um LLM orquestra: a cada passo responde `delegar <rotulo>
  <tarefa>` (o agente roda com o planner iterativo) ou `responder: <final>`.
  Exige o campo `supervisor: <llm>`; rastro com `{ agente, tarefa, texto }`.
  No modo `mock`, delega uma vez para cada agente, na ordem declarada, e
  então responde.

Retorna `{ texto, rastro: [ { agente, texto } ] }`.

## Exemplo

[`../exemplos/agente.tilt`](../exemplos/agente.tilt) — ferramenta + RAG + agente.
[`../exemplos/copiloto.tilt`](../exemplos/copiloto.tilt) — agente que revisa
código Tilt com o builtin `checar_tilt` (ver [guia 10](guia-10-ia-editores.md)).

```bash
TILT_LLM=mock tilt executar exemplos/agente.tilt
```
