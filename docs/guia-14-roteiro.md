# 14 — Roteiro (o que falta para excelência)

Levantamento do estado real (parser × checker × interpretador × runtime) e
dos gaps por domínio para a Tilt ficar excelente em engenharia de dados,
machine learning, deep learning, LLM, LLMOps e MLOps. Estado: Fases 12-5a,
12-6, 12-7 e 12-8 concluídas no escopo atual; kernels CUDA adicionais ainda
podem ampliar a cobertura de operadores.

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

## Engenharia de dados — base sólida, com compactação e escala em evolução

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
- **Qualidade de dados**: `verificar` valida e `quarentena:` já desvia linhas
  ruins em iterações; `perfil`, inferência amostrada e contratos versionados
  (`inferir_schema`, `validar_schema`, `evoluir_schema`) agora cobrem a entrada.
- **Limpeza de dados**: feito — `remover_nulos`, `preencher_nulos`, `renomear`,
  `remover_colunas`, `converter` (inclui `data`), `deduplicar`, `juntar`, `empilhar`,
  `descrever`, `amostra`, `contar_valores`, `limpar_texto` e `ordenar_por` com várias
  colunas, `ler_csv` com `separador:`/`pular:`/`nulos:`/`tipos:`/`inferir:`/`fuso:`/
  `sem_cabecalho:`,
  `escrever_csv` com aspas RFC 4180, `converter_data`/`ano`/`mes`/`dia`/`adicionar_dias`/
  `dias_entre` e `coalescer`, mais `pivotar`, `despivotar`, funções de janela
  (`janela`), `dividir_coluna` e `converter_fuso` (guia 03). A validação entre
  serviços externos ainda depende de publicar e distribuir o JSON do contrato.
- **Escrita analítica**: Delta/Iceberg particionam, compactam com
  `otimizar_delta`/`otimizar_iceberg`, z-order determinístico via `z_order:` e
  `vacuum_*` conservador para Parquet órfão; `ordenar_por` continua disponível
  para tabelas em memória.

## Machine learning clássico — feito (1ª passada)

- `experimento` executa de verdade: `regressao_linear`, `regressao_logistica`
  binária e multinomial, `knn`, `kmeans`, `floresta_aleatoria`,
  `gradiente_impulsionado` e `svm`, com `prever` (`{classe, probabilidade}` /
  `{valor}` / `{grupo}`), divisão treino/validação/teste com semente,
  `validacao_cruzada:`, `imputar:`, métricas (acurácia, f1 ponderado, auc,
  matriz_confusao, rmse, r2, inércia) e `registrar_em: mlflow://` via
  Tracking REST. Detalhes e limites no guia 04 e no guia 12.
- Busca em grade, aleatória e bayesiana existe (`estrategia: bayesiana` usa
  aquisição adaptativa sobre a grade finita).

## Deep learning — treino real, mas de brinquedo

Funciona: `treino` em CPU com camadas `densa`, `residual`, `incorporacao`, `recorrente` (RNN/LSTM/GRU), `conv2d`, `norma_lote`,
`agrupamento_max` e `achatar`, adam/SGD, perdas entropia
cruzada/quadrática, autograd manual.

- **Bloco residual**: `residual` preserva a largura da entrada e treina o ramo `x @ W + b` com SGD/Adam, mantendo a conexão de atalho.
- **CNN de brinquedo**: `conv2d` (com viés), `norma_lote` (gama/beta +
  média/variância correntes), `agrupamento_max` e `achatar` treinam de
  verdade (mini-lotes em CPU). `incorporacao` também treina a tabela por SGD/Adam;
  com recorrência RNN/LSTM/GRU e BPTT; `abandono` (dropout) atua no treino com máscara determinística pela semente.
- **GPU CUDA validada** em RTX 5050 para GEMM FP32, GEMM com operandos FP16,
  `conv2d`, ReLU, GELU e soma. O runtime também expõe residência f32,
  GEMM em lote, backward denso, convolução, recorrência e embeddings,
  normalização, pooling e redução; o treino escolhe CUDA quando elegível e
  mantém fallback CPU. `precisao: mista` usa GEMM FP16 com acumulação FP32.
- **Exportação**: `modelo <Nome>.exportar_onnx "modelo.onnx"` existe no
  interpretador (ONNX opset 20, sem dependências; ver guia 04) — o modelo
  treinado sai da Tilt para qualquer runtime ONNX. `salvar_pesos`/`carregar_pesos` também aceitam `.onnx` para persistir e restaurar inicializadores FLOAT32, incluindo recorrentes. `exportar_gguf` também grava
  GGUF v3 em F32 ou Q8_0 (`quantizacao:`), e `carregar_pesos` importa e
  desquantiza esses tensores.
- **Feito (treino utilizável)**: mini-lotes (`lote:`) com embaralhamento,
  `semente:` reproduzível (init + embaralhamento), checkpoint com retomada
  (`checkpoint:`/`a_cada:`/`retomar:`, bit-idêntico ao contínuo), agendador
  de taxa (`cosseno`/`degrau`), `validacao:` + `parar_cedo:` (restaura
  melhores pesos), `busca` em grade com `criterio:`, dataloader streaming de CSV e Parquet (`carregador ..., fluxo: verdadeiro` + `bloco:`)
  e exportação `gguf` (v3 F32/Q8_0), Safetensors F32 e exportação ONNX das
  recorrentes, residuais e da incorporação inicial.
- **Busca**: `estrategia: bayesiana` usa aquisição adaptativa sobre uma grade
  finita; grade e amostragem aleatória continuam disponíveis.

## LLM / RAG — funcional com contabilidade e avaliação

Funciona: cliente real anthropic/openai via curl, saída estruturada via
JSON Schema derivado de `tipo`, streaming SSE, embeddings, 6 backends
vetoriais. Robustez entregue: `tempo_limite`, `tentativas` com backoff (incluindo
`Retry-After` em 429), `reserva:`, `teto_tokens:` e `tokens:` na resposta.

- **Robustez**: timeout configurável, fallback entre modelos, cache opt-in e teto de
  tokens acumulado por `llm`. `contabilidade:` persiste tokens/custos em JSONL e
  `observabilidade:` registra eventos com prompts opt-in; `llm_metricas` consulta
  os totais mesmo depois de reiniciar.
- **RAG**: `dividir_texto`/`fragmentar` oferecem modos de sentença e código;
  `.avaliar` executa a busca no backend selecionado e calcula recall, precisão,
  MRR e nDCG.
- **Evals**: bloco `avaliacao` em 1ª passada (dataset em `dados:`, métricas
  `exata`/`contem`/`regex`/`tolerancia`/`juiz`, gate no `limiar:`,
  `amostra:` + `semente:`, `registrar_em:` local ou via MLflow REST; ver guia 05 e
  guia 12). Voto multi-juiz e amostra estratificada existem; os detalhes de
  cada caso são publicados no artefato `detalhes.json` do MLflow.

## Agentes — loop com políticas compartilháveis

Funciona: ciclo pensar→agir→observar, `max_passos`, memórias, equipe com
supervisor.

- **Guardrails**: `max_tokens_sessao`, orçamento monetário, `requer_aprovacao` e
  `ferramentas_permitidas` podem ser definidos em `politica Nome:` e reutilizados
  por vários agentes; o menor limite local/política vence.
- **Sem traço estruturado**: `rastro` existe, mas sem spans/tempo por passo
  exportáveis (OpenTelemetry).
- **Contexto**: memória vetorial plugada, mas sem compactação/resumo —
  conversa longa estoura a janela sem aviso.

## MLOps / servir — deployment frágil

Funciona: `servico` com epoll, arenas por requisição, rotas paralelas.
O despacho concorrente reutiliza `tilt::rt::ThreadPool`, com fila protegida, rejeição opcional por limite e desligamento gracioso; o smoke test `thread_pool` cobre submissão, drenagem e shutdown.

- `/saude`, `/metricas` (com latência por rota), graceful shutdown, limite
  global de requisição de 1 MiB e até 256 conexões existem; faltam limites
  configuráveis por rota e versionamento de modelo servido (A/B, canário,
  rollback).
- O treino pode registrar métricas, parâmetros e o vínculo do modelo em
  `registrar_em: mlflow://...`. O CLI também oferece um registry local
  (`registrar-modelo`/`listar-modelos`/`promover-modelo`/`resolver-modelo`) com
  versões, stages e SHA-256; `servir --pesos` permite conectar uma versão
  resolvida ao serviço. Ainda faltam lineage completo e rollout A/B, canário ou
  rollback automático no `servico`.
- `experimento Nome.prever_lote` faz inferência offline em tabela/lista,
  preservando a ordem e reutilizando o modelo ajustado. `modelo
  Nome.prever_lote(tabela, colunas: [...])` executa redes de entrada vetorial
  em um só lote e devolve uma tabela de saídas.

## Fundação SQL (em andamento)

- Feito (Marco 3/D1): `executar_sql` com placeholders `?` + `transacao`
  atômica nos 5 relacionais (postgres/sqlite/duckdb/mysql/clickhouse).
- Feito: **SELECT com `?`** (`consultar_sql url, sql [, params]` → tabela;
  mesma ligação do D1; sem `params` equivale à `consulta:` da `fonte`).
- Feito: **prepared server-side no MySQL** (`mysql_stmt_*` com `MYSQL_BIND`
  espelhado; layout comum a libmysqlclient e libmariadb validado contra as
  duas com servidor MariaDB 11; interpolação com escape removida).

## Sharding e escalabilidade

O bloco treino aceita num_shards: N e shard_id: K para selecionar linhas em
round-robin (K em 0..N-1). O contrato vale para dados em RAM e para os
dataloaders CSV/Parquet em fluxo; cada processo pode receber um shard
deterministico sem materializar o arquivo inteiro.

### Cluster mode de treinamento

Para treinar em processos separados sobre um filesystem compartilhado, use
`cluster: { dir: "...", rank: K, mundo: N, timeout: S }` dentro de `treino`.
Cada rank recebe automaticamente o shard correspondente e, ao fim de cada
época, publica um checkpoint, aguarda os demais e aplica a média dos pesos,
biases e estados recorrentes. O rank `0` publica o checkpoint agregado. Todos
os ranks devem usar o mesmo modelo, hiperparâmetros e diretório compartilhado;
o `timeout` evita espera infinita. Os momentos do Adam e as estatísticas de
normalização permanecem locais nesta primeira versão, portanto o modo é uma
sincronização de parâmetros por época (não um all-reduce de gradientes).
No fluxo Parquet com pelo menos um row group por rank, o cluster atribui
grupos inteiros em round-robin e pula os grupos alheios durante as épocas.
Com menos grupos que ranks, usa a divisão por linha. A primeira passada ainda
percorre os grupos para ler os rótulos, mas projeta apenas a coluna alvo;
durante as épocas, lê apenas os atributos necessários. `num_shards`/`shard_id`
sem `cluster` preservam a divisão por linha.
Para diagnosticar o custo de leitura e sincronização, `TILT_CLUSTER_PROFILE=1`
registra no stderr os tempos `labels_ms` e `sync_epoch_ms` de cada rank.

Também foi concluído o query pushdown das fontes SQL: `pushdown.colunas`,
`pushdown.onde` (igualdade parametrizada, inclusive nulo) e `pushdown.limite`
são aplicados em SQLite, Postgres, DuckDB, MySQL/MariaDB e ClickHouse, com
teste CTest local e sem interpolar valores no SQL.

## Desempenho

Medições, causas e o plano para acelerar lógica e dados estão no
[guia 16](guia-16-desempenho.md): `Value` compacto, variáveis por slot, chamadas
baratas, tabela colunar, leitura paralela e delegação ao DuckDB. Já entregues: leitura de
CSV/Parquet em paralelo, `ordenar_por` por índices, chamadas VM → VM diretas e `sql` com
DuckDB (ver "O que já foi feito" no guia 16).

## Priorizacao sugerida

1. ~~`experimento` executável~~ feito (1ª passada; ver acima).
2. ~~Robustez LLM~~ feito (1ª passada; ver acima).
3. ~~Operação de pipelines~~ feito (1ª passada: `ao_falhar` com
   `espera:`/`backoff:`, `tempo_limite:` por passo, `quarentena:` no
   `para cada`, `saude:`/`metricas:` no `servico`, latências por rota no
   `/metricas`, logs estruturados, cursor/backfill e `vacuum_*`).
4. ~~`exportar: onnx`~~ feito (`modelo <Nome>.exportar_onnx "modelo.onnx"`; ver guia 04).
5. ~~`avaliacao` (evals — fundação de LLMOps)~~ feito em 1ª passada (ver guia 05).
6. Fechar os contratos de linguagem (tipos, formas e módulos) e eliminar
   divergências entre checker e runtime. O LSP já navega importações explícitas
   locais; índice de workspace, referências e rename entre arquivos faltam.
7. Reduzir materialização de tabelas e custo de `Value`; medir com
   `bench/comparar.py` antes de alterar a representação.
8. Completar MLOps: stages, lineage e rollout de serviço sobre o registry local
   e a integração MLflow.
9. Melhorar GPU com residência persistente de tensores e kernels CUDA para
   normalização/treino. O backward denso, de convolução, recorrência e
   embeddings já tem despacho CUDA; Metal e ambientes sem CUDA continuam com
   fallback CPU. O próximo ganho é reduzir cópias mantendo ativações residentes.
10. CI/CD: o workflow `ci` executa lint, build e CTest em pushes para `main`/
    `master`, pull requests e disparo manual. O job `performance` roda
    `bench/comparar.py` para detectar regressões relativas ao baseline da mesma
    máquina; publicação e deploy continuam fora deste escopo.
