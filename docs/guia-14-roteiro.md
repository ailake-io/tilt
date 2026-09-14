# 14 — Roteiro (o que falta para excelência)

Levantamento do estado real (parser × checker × interpretador × runtime) e
dos gaps por domínio para a Tilt ficar excelente em engenharia de dados,
machine learning, deep learning, LLM, LLMOps e MLOps. Estado: pós-Marco 3/D1
(`executar_sql` com `?` + `transacao`; ver guia 03).

## Engenharia de dados — base sólida, faltam operação e escala

Funciona: CSV/JSON/Parquet/Delta/Iceberg, 20+ conectores, `pipeline`,
`verificar`, `janela`, `--agendar` com checkpoint e eleição de líder.

- **Orquestração de verdade**: `ao_falhar: repetir N` existe, mas sem
  retries com backoff/jitter, timeout por passo, callbacks
  `on_success`/`on_failure`, SLA com alerta. Sem isso, pipeline em
  produção é cego.
- **Observabilidade**: sem endpoint `/saude` ou `/metricas`, sem log
  estruturado (JSON) com `trace_id` por execução, sem dead-letter (linhas
  que falham somem ou abortam tudo).
- **Incremental/backfill**: `janela` + offset cobrem streaming simples, mas
  falta carga incremental por cursor (`desde: <coluna>`, watermark) e
  reprocessamento de intervalo (backfill) — o pão com manteiga de DE.
- **Qualidade de dados**: `verificar` valida, mas sem quarentena (desviar
  linhas ruins), sem perfilagem/estatísticas, sem contrato de schema
  versionado na entrada.
- **Escrita analítica**: Delta/Iceberg particionam, mas falta
  `ordenar_por`/z-order e compactação (`vacuum`/`optimize`) — tabelas
  degradam com o tempo.

## Machine learning clássico — maior buraco: `experimento` não executa

- `experimento` é **só sintaxe** (parseado, zero referências no
  interpretador/VM/IR). `floresta_aleatoria`, `f1`, `matriz_confusao`,
  `registrar_em: mlflow` — nada roda. Item nº 1 para a linguagem cumprir
  a promessa de ML: pelo menos **regressão linear/logística + kmeans +
  kNN executando de verdade**, com `prever`, divisão
  treino/validação/teste e métricas (acurácia, f1, auc).
- Sem isso, o `servico Predicao` do guia 06 é ficção.

## Deep learning — treino real, mas de brinquedo

Funciona: `treino` em CPU com camadas `densa`, adam/SGD, perdas entropia
cruzada/quadrática, autograd manual.

- **Camadas que faltam no `modelo`/`treino`**: `conv2d`/`norma_lote` são só
  ops avulsas (sem backward, sem pesos) — CNN não treina. Sem
  `incorporacao` treinável, sem recorrência, sem abandono no treino.
- **GPU não validada** (`TILT_GPU=fake` em CPU; CUDA nunca rodou em
  hardware real) + sem AMP real.
- **Exportação zero**: `modelo.exportar: onnx/gguf` não existe no
  interpretador — o modelo treinado fica preso na Tilt. Sem ONNX não há
  deploy fora dela.
- **Faltam**: dataloader completo, checkpoint de treino com retomada
  (incl. estado do otimizador), busca de hiperparâmetros, seed
  reproduzível.

## LLM / RAG — funcional, falta engenharia de produção

Funciona: cliente real anthropic/openai via curl, saída estruturada via
JSON Schema derivado de `tipo`, streaming SSE, embeddings, 6 backends
vetoriais.

- **Robustez**: sem retry com backoff em 429/5xx, timeout configurável,
  fallback entre modelos, cache de respostas, teto de custo/tokens por
  período. Hoje um 429 aborta tudo.
- **RAG**: sem reranking, chunking só de tamanho fixo (sem respeito a
  sentença/código), sem busca híbrida (vetor + keyword/BM25), sem
  avaliação de recuperação.
- **Evals**: zero. Sem bloco `avaliacao` (dataset ouro + métrica + limiar)
  não há LLMOps — ninguém itera prompt sem eval.
- **Observabilidade LLM**: sem log de prompts/respostas nem contagem de
  tokens e custo por chamada/fluxo.

## Agentes — loop existe, falta controle

Funciona: ciclo pensar→agir→observar, `max_passos`, memórias, equipe com
supervisor.

- **Sem guardrails**: sem limite de custo/tokens por sessão, sem aprovação
  humana antes de ferramenta destrutiva, sem allowlist de ferramentas por
  ambiente com enforcement.
- **Sem traço estruturado**: `rastro` existe, mas sem spans/tempo por passo
  exportáveis (OpenTelemetry).
- **Contexto**: memória vetorial plugada, mas sem compactação/resumo —
  conversa longa estoura a janela sem aviso.

## MLOps / servir — deployment frágil

Funciona: `servico` com epoll, arenas por requisição, rotas paralelas.

- Sem `/saude`, `/metricas`, graceful shutdown, limite de
  payload/concorrência por rota, versionamento de modelo servido (A/B,
  canário, rollback).
- Sem registro de modelos: `pesos:` é arquivo solto — sem registry
  (nome/versão/stage, lineage experimento→modelo→serviço).
- Sem inferência em lote (rodar o modelo sobre uma tabela inteira,
  offline) como primitiva — só rota online.

## Fundação SQL (em andamento)

- Feito (Marco 3/D1): `executar_sql` com placeholders `?` + `transacao`
  atômica nos 5 relacionais (postgres/sqlite/duckdb/mysql/clickhouse).
- Feito: **SELECT com `?`** (`consultar_sql url, sql [, params]` → tabela;
  mesma ligação do D1; sem `params` equivale à `consulta:` da `fonte`).
- Feito: **prepared server-side no MySQL** (`mysql_stmt_*` com `MYSQL_BIND`
  espelhado; layout comum a libmysqlclient e libmariadb validado contra as
  duas com servidor MariaDB 11; interpolação com escape removida).

## Priorização sugerida

1. `experimento` executável (ML clássico mínimo + métricas).
2. Robustez LLM (retry/backoff/timeout/fallback + tokens/custo).
3. Operação de pipelines (timeout por passo, backoff, quarentena,
   `/saude` + `/metricas`).
4. `exportar: onnx`.
5. `avaliacao` (evals — fundação de LLMOps).
