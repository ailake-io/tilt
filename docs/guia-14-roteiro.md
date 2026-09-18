# 14 — Roteiro (o que falta para excelência)

Levantamento do estado real (parser × checker × interpretador × runtime) e
dos gaps por domínio para a Tilt ficar excelente em engenharia de dados,
machine learning, deep learning, LLM, LLMOps e MLOps. Estado: Fase 12-6.5
concluída; a próxima frente é fechar a operação de pipelines.

## Fase 12-5a — formatos e formas de tensor

As entregas .1–.5 estão cobertas pelos testes de Parquet/Iceberg e a .6 pelo
checker semântico e seus goldens. O estado consolidado é:

- Parquet: listas aninhadas e listas recursivas de structs, `field_id`,
  decimais grandes e UUID.
- Iceberg: transforms com poda, partition summaries, sequence numbers e
  equality deletes na leitura.
- Shape solver: contratos de retorno tensor em funções locais com `_`,
  instanciação pelas dimensões conhecidas dos argumentos e propagação para
  atribuições diretas como `m.campo = tensor`.
- A .7 foi concluída com golden de regressão, documentação revisada e commit
  isolado das alterações da fase.

## Engenharia de dados — base sólida, faltam operação e escala

Funciona: CSV/JSON/Parquet/Delta/Iceberg, 20+ conectores, `pipeline`,
`verificar`, `janela`, `--agendar` com checkpoint e eleição de líder.

- **Orquestração de verdade**: `ao_falhar: repetir N` com `espera:`,
  `backoff:` e jitter, `tempo_limite:` por passo, callbacks
  `on_success`/`on_failure` e alerta de SLA existem.
- **Observabilidade**: `saude:`/`metricas:` no `servico` existem; o log de
  cada requisição é JSON compacto com `trace_id`, status e latência, e o
  `/metricas` reporta latência monotônica total/média/máxima por rota;
  `/metricas/prometheus` exporta contadores e latência por labels.
  `TILT_PIPELINE_LOG_JSON=1` adiciona contexto de execução dos pipelines.
- **Incremental/backfill**: `janela` + offset cobrem streaming simples; `desde:
  <coluna>` adiciona watermark numérico/textual persistente e
  `backfill: { desde: valor, ate: valor }` permite reprocessar um intervalo
  inclusivo sem avançar o cursor.
- **Qualidade de dados**: `verificar` valida, mas sem quarentena (desviar
  linhas ruins), sem perfilagem/estatísticas, sem contrato de schema
  versionado na entrada.
- **Escrita analítica**: Delta/Iceberg particionam e já têm `vacuum_*`
  conservador para Parquet órfão; falta compactação (`optimize`) e
  `ordenar_por`/z-order.

## Machine learning clássico — feito (1ª passada)

- `experimento` executa de verdade: `regressao_linear`, `regressao_logistica`
  binária e multinomial, `knn`, `kmeans`, `floresta_aleatoria`,
  `gradiente_impulsionado` e `svm`, com `prever` (`{classe, probabilidade}` /
  `{valor}` / `{grupo}`), divisão treino/validação/teste com semente,
  `validacao_cruzada:`, `imputar:`, métricas (acurácia, f1 ponderado, auc,
  matriz_confusao, rmse, r2, inércia) e `registrar_em: mlflow://` via
  Tracking REST. Detalhes e limites no guia 04 e no guia 12.
- Resta: busca de hiperparâmetros.

## Deep learning — treino real, mas de brinquedo

Funciona: `treino` em CPU com camadas `densa`, `residual`, `incorporacao`, `recorrente` (RNN/LSTM/GRU), `conv2d`, `norma_lote`,
`agrupamento_max` e `achatar`, adam/SGD, perdas entropia
cruzada/quadrática, autograd manual.

- **Bloco residual**: `residual` preserva a largura da entrada e treina o ramo `x @ W + b` com SGD/Adam, mantendo a conexão de atalho.
- **CNN de brinquedo**: `conv2d` (com viés), `norma_lote` (gama/beta +
  média/variância correntes), `agrupamento_max` e `achatar` treinam de
  verdade (mini-lotes em CPU). `incorporacao` também treina a tabela por SGD/Adam;
  com recorrência RNN/LSTM/GRU e BPTT; abandono ainda é identidade no treino.
- **GPU não validada** (`TILT_GPU=fake` em CPU; CUDA nunca rodou em
  hardware real) + sem AMP real.
- **Exportação**: `modelo <Nome>.exportar_onnx "modelo.onnx"` existe no
  interpretador (ONNX opset 20, sem dependências; ver guia 04) — o modelo
  treinado sai da Tilt para qualquer runtime ONNX. `salvar_pesos`/`carregar_pesos` também aceitam `.onnx` para persistir e restaurar inicializadores FLOAT32, incluindo recorrentes. `exportar_gguf` também grava
  GGUF v3 em F32; quantização e importação ainda não existem.
- **Feito (treino utilizável)**: mini-lotes (`lote:`) com embaralhamento,
  `semente:` reproduzível (init + embaralhamento), checkpoint com retomada
  (`checkpoint:`/`a_cada:`/`retomar:`, bit-idêntico ao contínuo), agendador
  de taxa (`cosseno`/`degrau`), `validacao:` + `parar_cedo:` (restaura
  melhores pesos), `busca` em grade com `criterio:`, dataloader streaming de CSV e Parquet (`carregador ..., fluxo: verdadeiro` + `bloco:`)
  e exportação `gguf` (v3, só escrita), Safetensors F32 e exportação ONNX das recorrentes e residuais.
- **Faltam**: busca de hiperparâmetros além de grade (random/bayesiana),
  `gguf` com quantização (hoje só F32).

## LLM / RAG — funcional, falta engenharia de produção

Funciona: cliente real anthropic/openai via curl, saída estruturada via
JSON Schema derivado de `tipo`, streaming SSE, embeddings, 6 backends
vetoriais. Robustez entregue: `tempo_limite`, `tentativas` com backoff (incluindo
`Retry-After` em 429), `reserva:`, `teto_tokens:` e `tokens:` na resposta.

- **Resta de robustez**: cache de respostas e retry em streaming.

- **Robustez**: timeout configurável, fallback entre modelos e teto de
  custo/tokens por período. `Retry-After` em 429 já é respeitado; ainda faltam
  cache de respostas e retry em streaming.
- **RAG**: sem reranking, chunking só de tamanho fixo (sem respeito a
  sentença/código), sem busca híbrida (vetor + keyword/BM25), sem
  avaliação de recuperação.
- **Evals**: bloco `avaliacao` em 1ª passada (dataset em `dados:`, métricas
  `exata`/`contem`/`regex`/`tolerancia`/`juiz`, gate no `limiar:`,
  `amostra:` + `semente:`, `registrar_em:` local ou via MLflow REST; ver guia 05 e
  guia 12). Restam juiz com voto e amostra por fração.
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

- `/saude`, `/metricas` (com latência por rota) e graceful shutdown existem;
  faltam limite de payload/concorrência por rota e versionamento de modelo
  servido (A/B, canário, rollback).
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
   `para cada`, `saude:`/`metricas:` no `servico`, latências por rota no
   `/metricas`; sem log estruturado com `trace_id`, Retry-After/cache no LLM).
4. ~~`exportar: onnx`~~ feito (`modelo <Nome>.exportar_onnx "modelo.onnx"`; ver guia 04).
3. Operação de pipelines (timeout por passo, backoff, quarentena,
   `/saude` + `/metricas`).
4. ~~`exportar: onnx`~~ feito (ver item 4 acima).
5. ~~`avaliacao` (evals — fundação de LLMOps)~~ feito em 1ª passada (ver guia 05).
