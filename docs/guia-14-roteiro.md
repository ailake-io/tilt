# 14 — Roteiro (o que falta para excelência)

Levantamento do estado real (parser × checker × interpretador × runtime) e
dos gaps por domínio para a Tilt ficar excelente em engenharia de dados,
machine learning, deep learning, LLM, LLMOps e MLOps. Estado: pós-Marco 3/D1
(`executar_sql` com `?` + `transacao`; ver guia 03).

## Engenharia de dados — base sólida, faltam operação e escala

Funciona: CSV/JSON/Parquet/Delta/Iceberg, 20+ conectores, `pipeline`,
`verificar`, `janela`, `--agendar` com checkpoint e eleição de líder.

- **Orquestração de verdade**: `ao_falhar: repetir N` com `espera:` e
  `backoff:` e `tempo_limite:` por passo existem; restam callbacks
  `on_success`/`on_failure`, SLA com alerta e jitter no backoff.
- **Observabilidade**: `saude:`/`metricas:` no `servico` existem; restam
  log estruturado (JSON) com `trace_id` por execução e latências no
  `/metricas`. Dead-letter existe via `quarentena:` no `para cada`.
- **Incremental/backfill**: `janela` + offset cobrem streaming simples, mas
  falta carga incremental por cursor (`desde: <coluna>`, watermark) e
  reprocessamento de intervalo (backfill) — o pão com manteiga de DE.
- **Qualidade de dados**: `verificar` valida, mas sem quarentena (desviar
  linhas ruins), sem perfilagem/estatísticas, sem contrato de schema
  versionado na entrada.
- **Escrita analítica**: Delta/Iceberg particionam, mas falta
  `ordenar_por`/z-order e compactação (`vacuum`/`optimize`) — tabelas
  degradam com o tempo.

## Machine learning clássico — feito (1ª passada)

- `experimento` executa de verdade: `regressao_linear`, `regressao_logistica`
  binária e multinomial, `knn`, `kmeans`, `floresta_aleatoria`,
  `gradiente_impulsionado` e `svm`, com `prever` (`{classe, probabilidade}` /
  `{valor}` / `{grupo}`), divisão treino/validação/teste com semente,
  `validacao_cruzada:`, `imputar:`, métricas (acurácia, f1 ponderado, auc,
  matriz_confusao, rmse, r2, inércia) e `registrar_em: mlflow://` como JSON
  local. Detalhes e limites no guia 04 e no guia 12.
- Resta: mlflow REST, busca de hiperparâmetros.

## Deep learning — treino real, mas de brinquedo

Funciona: `treino` em CPU com camadas `densa`, `conv2d`, `norma_lote`,
`agrupamento_max` e `achatar`, adam/SGD, perdas entropia
cruzada/quadrática, autograd manual.

- **CNN de brinquedo**: `conv2d` (com viés), `norma_lote` (gama/beta +
  média/variância correntes), `agrupamento_max` e `achatar` treinam de
  verdade (lote cheio, CPU). Sem `incorporacao` treinável, sem recorrência,
  sem abandono no treino, sem dilation/padding explícito, sem mini-lotes.
- **GPU não validada** (`TILT_GPU=fake` em CPU; CUDA nunca rodou em
  hardware real) + sem AMP real.
- **Exportação**: `modelo <Nome>.exportar_onnx "modelo.onnx"` existe no
  interpretador (ONNX opset 20, sem dependências; ver guia 04) — o modelo
  treinado sai da Tilt para qualquer runtime ONNX. Resta `gguf`.
- **Feito (treino utilizável)**: mini-lotes (`lote:`) com embaralhamento,
  `semente:` reproduzível (init + embaralhamento), checkpoint com retomada
  (`checkpoint:`/`a_cada:`/`retomar:`, bit-idêntico ao contínuo), agendador
  de taxa (`cosseno`/`degrau`), `validacao:` + `parar_cedo:` (restaura
  melhores pesos), `busca` em grade com `criterio:`, dataloader streaming
  de CSV (`carregador ..., fluxo: verdadeiro` + `bloco:`) e exportação
  `gguf` (v3, só escrita).
- **Faltam**: dataloader de arquivos grandes em outros formatos (Parquet),
  busca de hiperparâmetros além de grade (random/bayesiana), `gguf` com
  quantização (hoje só F32).

## LLM / RAG — funcional, falta engenharia de produção

Funciona: cliente real anthropic/openai via curl, saída estruturada via
JSON Schema derivado de `tipo`, streaming SSE, embeddings, 6 backends
vetoriais. Robustez entregue (1ª passada): `tempo_limite`, `tentativas`
com backoff, `reserva:`, `teto_tokens:` e `tokens:` na resposta.

- **Resta de robustez**: `Retry-After` em 429, cache de respostas e
  retry em streaming.

- **Robustez**: sem retry com backoff em 429/5xx, timeout configurável,
  fallback entre modelos, cache de respostas, teto de custo/tokens por
  período. Hoje um 429 aborta tudo.
- **RAG**: sem reranking, chunking só de tamanho fixo (sem respeito a
  sentença/código), sem busca híbrida (vetor + keyword/BM25), sem
  avaliação de recuperação.
- **Evals**: bloco `avaliacao` em 1ª passada (dataset em `dados:`, métricas
  `exata`/`contem`/`regex`/`tolerancia`/`juiz`, gate no `limiar:`,
  `amostra:` + `semente:`, `registrar_em:` em JSON local; ver guia 05 e
  guia 12). Restam juiz com voto, amostra por fração e mlflow REST.
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

1. ~~`experimento` executável~~ feito (1ª passada; ver acima).
2. ~~Robustez LLM~~ feito (1ª passada; ver acima).
3. ~~Operação de pipelines~~ feito (1ª passada: `ao_falhar` com
   `espera:`/`backoff:`, `tempo_limite:` por passo, `quarentena:` no
   `para cada`, `saude:`/`metricas:` no `servico`; sem latências no
   `/metricas`, sem Retry-After/cache no LLM).
4. ~~`exportar: onnx`~~ feito (`modelo <Nome>.exportar_onnx "modelo.onnx"`; ver guia 04).
3. Operação de pipelines (timeout por passo, backoff, quarentena,
   `/saude` + `/metricas`).
4. ~~`exportar: onnx`~~ feito (ver item 4 acima).
5. ~~`avaliacao` (evals — fundação de LLMOps)~~ feito em 1ª passada (ver guia 05).
