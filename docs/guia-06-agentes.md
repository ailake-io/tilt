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
- Os campos declarados são obrigatórios nas chamadas diretas; cada valor é validado contra texto, inteiro, decimal, logico, lista[...], mapa, tabela, tensor e uniões literais. Campo desconhecido ou tipo incompatível gera erro capturável por tentar.
- No planner, os campos ausentes continuam sendo preenchidos best-effort antes da validação; isso preserva o fallback documentado para agentes.
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
chamadas seguintes. `memoria: vetorial` guarda cada turno (pergunta+resposta)
num índice em memória e prefixa os 3 mais similares (`Lembretes relevantes:`)
— usa `embeddings:` do agente (default `text-embedding-3-small`; no mock,
vetores determinísticos de 16 dimensões). Sem poda: acima de 200 turnos por
agente, turnos novos não entram. Outro valor em `memoria:` é erro (`T011`).
No modo `TILT_LLM=mock` o planner é determinístico: cada
ferramenta é chamada uma vez, na ordem declarada, e depois o mock responde.

### Tool-calling nativo (`protocolo: nativo`)

Com `protocolo: nativo` no `agente`, o interpretador usa o tool-calling do
próprio provedor em vez do protocolo de texto acima: as ferramentas vão como
`tools` (Anthropic: `input_schema`; OpenAI/local/vLLM: `function.parameters`),
o schema JSON sai dos campos de `entrada:` (`texto`→string, `inteiro`→integer,
`decimal`→number, `logico`→boolean, `lista`→array, `mapa`→object) e o modelo
devolve chamadas estruturadas (`tool_use` / `tool_calls`). Cada resultado volta
ao modelo como `tool_result` / mensagem `role: tool` com o mesmo id, até haver
uma resposta sem chamadas ou `max_passos`. O padrão continua sendo
`protocolo: texto`. Em `TILT_LLM=mock` o planner chama cada ferramenta uma vez,
na ordem. Coberto por `tests/agent_native_test.sh` (servidor falso nos dois
formatos).

### Guardrails

- `ferramenta` com `requer_aprovacao: verdadeiro` só roda depois de uma
  aprovação humana. `TILT_APROVAR=sim` (ou `todas`) aprova, `TILT_APROVAR=nao`
  nega; sem a variável, o `tilt` pergunta no terminal (`Executar? [s/N]`) e, sem
  terminal interativo, nega. Uma ferramenta negada não aborta o agente: a
  observação vira `[negada] <motivo>` e a entrada do `rastro` ganha
  `negada: verdadeiro`.
- `agente` com `max_tokens_sessao: N` para de chamar o LLM quando a soma de
  tokens (entrada + saída) de uma chamada de `.responder` chega a `N` e responde
  `[agente] limite de tokens da sessao (N) atingido`. `0`/ausente = sem teto.

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
- `paralelo` — todos recebem a mesma mensagem e rodam **ao mesmo tempo** (uma thread por agente; o tempo total é o do mais lento); retorna `rotulo: texto` concatenado, na ordem declarada, e o `rastro` na mesma ordem. O primeiro erro (na ordem) aborta depois de todos terminarem. Coberto por `tests/equipe_paralela_test.sh`.
- `supervisor` — um LLM orquestra: a cada passo responde `delegar <rotulo>
  <tarefa>` (o agente roda com o planner iterativo) ou `responder: <final>`.
  Exige o campo `supervisor: <llm>`; rastro com `{ agente, tarefa, texto }`.
  No modo `mock`, delega uma vez para cada agente, na ordem declarada, e
  então responde.

Retorna `{ texto, rastro: [ { agente, texto } ] }`.

## Exemplo

[`../exemplos/agente.tilt`](../exemplos/agente.tilt) — ferramenta + RAG + agente.
[`../exemplos/agente_multi_etapas.tilt`](../exemplos/agente_multi_etapas.tilt) —
equipe supervisor com busca vetorial e validação real via `checar_tilt`.
[`../exemplos/copiloto.tilt`](../exemplos/copiloto.tilt) — agente que revisa
código Tilt com o builtin `checar_tilt` (ver [guia 10](guia-10-ia-editores.md)).

```bash
TILT_LLM=mock tilt executar exemplos/agente.tilt
```
