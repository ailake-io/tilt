# Tarefas do Projeto Tilt

> Atualizado em 2026-09-18 com base no estado do repositório.

## Legenda
- 🔴 **P0** — Crítico / Bloqueante
- 🟠 **P1** — Importante / Alto valor
- 🟡 **P2** — Recomendado / Melhoria
- 🟢 **P3** — Desejável / Nice to have

## Status atual — Fase 12

### Fase 12-5a
- [x] 12-5a.1 — Parquet: 3+ níveis de lista e listas de structs recursivas
- [x] 12-5a.2 — Parquet: `field_ids`, decimais grandes e UUID
- [x] 12-5a.3 — Iceberg: transforms com poda
- [x] 12-5a.4 — Iceberg: partition summaries e sequence numbers reais
- [x] 12-5a.5 — Iceberg: equality deletes na leitura
- [x] 12-5a.6 — Core shape solver dinâmico e member assignment (contratos de
  forma em funções locais com `_`, instanciação pelo argumento e propagação
  para `m.campo = tensor`)
- [x] 12-5a.7 — Goldens, documentação e commit da fase concluídos

### Próximas fases
- [x] Fase 12-6 — Operação de pipelines
- [x] 12-6.1 — Métricas HTTP com latência monotônica por rota
- [x] 12-6.2 — Log HTTP estruturado em JSON com `trace_id`
- [x] 12-6.3 — Cursor incremental com watermark persistente e backfill inclusivo
- [x] 12-6.4 — Exportação Prometheus de métricas HTTP
- [x] 12-6.5 — Callbacks de pipeline, alerta de SLA e jitter no backoff
- [x] 12-6.6 — Logs JSON opt-in com contexto de execução dos pipelines
- [x] 12-6.7 — Vacuum conservador de arquivos órfãos Delta/Iceberg
- [x] Fase 12-7 — CUDA para GEMM/conv2d/ReLU/GELU/soma + AMP de GEMM denso/residual, com pesos e gradientes FP32
- [x] Fase 12-8 — CI Windows com testes funcionais (build MSVC + CTest nativo via `tests/windows_functional.ps1`; conectores shell permanecem em Linux/macOS)
- [x] GPU — GEMM FP32/FP16, conv2d, ReLU, GELU e soma validados em RTX 5050 (CUDA 12.0)
- [x] CPU — CBLAS opcional para GEMM e conv2d grande, mantendo kernels portáteis
- [x] Inferência offline em lote de tabela para `experimento` e `modelo` vetorial
- [x] Benchmark detalhado CPU/GPU e comparação local com CPython, pandas e NumPy (`benchmarks/relatorio-2026-09-24.md`)
- [x] GPU — backend Metal (macOS), buffers residentes gerais (GEMM, batching,
  convolução, viés por canal, ativações, normalização, pooling e redução),
  `GpuGraph`, backward denso sem uploads intermediários e ativação de Tensor
  Core via cuBLAS quando disponível; AMD/ROCm permanece fora do escopo atual


---

## 1. Linguagem e Compilador

### 1.1 Infraestrutura de Testes (P1)
- [x] Expandir cobertura de testes dourados em `tests/golden/` para todas as novas features (149 casos verdes; novos: `chk-tipo-registro` [T033], `run-experimento-preco`, `run-experimento-prever`; helper `tests/new_golden.sh`)
- [x] Adicionar testes de regressão para bugs corrigidos (ex: T011, T012)
- [x] Criar teste automatizado de compatibilidade entre interpretador e VM (`tests/native_test.sh`, `tests/native_arm64_test.sh`) — CTest nativo e ARM64 passam; QEMU roda quando toolchain estiver disponível
- [x] Integrar testes de lint/estilo no CI (`.github/workflows/ci.yml`) — job incremental com `git diff --check` + `clang-format`

### 1.2 Parser e Semântica (P1)
- [x] Parse de `tipo` com valores padrão (Sprint 1: parseia, T011, aplica em formato/entrada)
- [x] Aridade de funções de usuário (Sprint 1: T011, faltantes sempre; sobra só em f(...))
- [x] Implementar inferência de tipos para fluxo condicional (T011 — merge conservador de tipos definidos em todos os ramos, com promoção inteiro→decimal)
- [x] Completar solver de formas para `atencao` dinâmica e `conv2d` com formas dinâmicas (T012 — ShapeEnv fundido entre ramos condicionais; dimensões divergentes viram `_`)

### 1.3 VM e Codegen (P1)
- [x] Expandir subconjunto VM para cobrir mais builtins (`ler_csv`, interpolação, membros)
  (Sprint 2: `Op::GetField` com paridade total — mapa/tabela, `.tamanho`, props
  de tensor, `?.` — interpolação `{{nome}}` sobre locais via `+`, `ler_csv`
  via `CallFunc`+hook; codegen fail-closed com rejeição clara; goldens
  `run-vm-interpola/membro/ler-csv` com paridade interp×VM verificada)
- [x] Implementar JIT (compilação em runtime sem passar por `.s`+`cc`) — backend x86-64 direto em memória para o subconjunto inteiro; fallback fail-closed para a VM; teste `jit_test.sh`
- [x] Adicionar testes end-to-end para codegen ARM64 em qemu (job `arm64_codegen` instala toolchain cross + qemu)
- [x] Cache de bytecode em disco (Sprint 3: `.tiltc`, SHA do fonte, fail-closed)

---

## 2. Runtime e Conectores

### 2.1 Conectores de Dados (P1)
- [x] Connector `delta` nativo (`src/runtime/delta.cpp`, escrita/leitura/append/evolução/widening)
- [x] Validar conectores PostgreSQL, MySQL, DuckDB, ClickHouse em CI real (job `connectors`, servidores/libs provisionados no runner)
- [x] Melhorar tratamento de erros em conectores TLS (redis/mongo/kafka) — certificados auto-assinados e diagnóstico preservado de `dlopen`/símbolo OpenSSL ausente
- [x] Adicionar pooling de conexões para bancos relacionais (Sprint 2:
  `src/runtime/sql_pool.*` genérico por (backend, url) — postgres/mysql/duckdb
  nos statements avulsos, `transacao` dedicada, BEGIN/START/SET descartam,
  `TILT_SQL_POOL*` envs; sqlite/clickhouse fora com motivo; teste
  `sql_pool_test.sh` + driver standalone sem servidor)

### 2.2 Parquet/Delta/Iceberg (P2)
- [x] Suportar 3+ níveis de lista no Parquet
- [x] Suportar structs com `field_ids` explícitos (caminho Iceberg)
- [x] Evolução de schema Delta (fase 27 add-column + widening int->long/float->double)
- [x] Evolução de schema Iceberg (fase 27 add-column + widening, ids estáveis)
- [x] Implementar REST catalog do Iceberg (fase 29)
- [x] Listas escalares Parquet nested com quatro ou mais níveis no leitor genérico
- [x] Provedores de chave Parquet por variável de ambiente e arquivo (além do AWS KMS)
- [x] Locks cooperativos e optimistic concurrency local para Delta/Iceberg
- [x] Paginação, Bearer token e HEAD no servidor REST Iceberg
- [x] Provedores HTTP Parquet para Azure Key Vault, GCP Cloud KMS e Vault Transit
- [x] Operações createTable, transactions e DELETE no servidor REST Iceberg
- [x] Implementar `tilt servir-catalogo` (fase 30)
- [x] Adicionar deletes (position/equality) para Iceberg (fase 12-5a; leitura
  nativa aplica ambos, pyiceberg aplica position e ainda não suporta equality)
- [x] Implementar compactação `optimize` para Delta/Iceberg (`otimizar_delta`/`otimizar_iceberg`)
- [x] Implementar z-order para escrita analítica particionada (`z_order:` nos writers Delta/Iceberg)

### 2.3 Streaming (P2)
- [x] Implementar streaming de Parquet no treino (`carregador ..., fluxo: verdadeiro`; CSV e Parquet)
- [x] Adicionar suporte a Kafka transactions multi-partição
- [x] Adicionar persistência de estado entre disparos cron (`--agendar`)

---

## 3. Machine Learning

### 3.1 Experimentos (P1)
- [x] `busca` em grade (já existia; validada na Sprint 1)
- [x] `imputar` no `pre_processar` (já existe)
- [x] `f1` ponderado pelo suporte (já existe)
- [x] `validacao_cruzada` (já existe)
- [x] Implementar `registrar_em: mlflow://` via Tracking REST

### 3.2 Treino (P1)
- [x] Implementar AMP em GEMM denso/residual no treino (`precisao: mista`; FP16 nos operandos, FP32 no acumulador/pesos/gradientes)
- [x] Implementar `ao_epoca` (callback por época; bloco com contexto da época)
- [x] Adicionar dataloader de Parquet para treino (row groups, fluxo 2D)
- [x] Implementar dilation e padding explícito em conv2d
- [x] Implementar dataloader com `shuffle` configurável (`embaralhar: verdadeiro|falso`)

### 3.3 Modelos (P2)
- [x] Adicionar camada `incorporacao` (embedding layer)
- [x] Adicionar camada `recorrente` (RNN/LSTM/GRU, BPTT CPU)
- [x] Adicionar camada `residual` (bloco treinável CPU, persistência e ONNX)
- [x] Implementar `salvar_pesos`/`carregar_pesos` em formato ONNX
- [x] Integrar Safetensors F32 para pesos de produção
- [x] Implementar exportação/importação GGUF v3 F32 e Q8_0, com desquantização no runtime
- [x] Exportar a camada `incorporacao` inicial para ONNX via `Gather` INT64
- [x] Adicionar busca bayesiana adaptativa sobre grades finitas (`estrategia: bayesiana`)

---

## 4. LLM e RAG

### 4.1 LLM (P1)
- [x] `TILT_LLM=mock` (já existia; verificado na Sprint 2)
- [x] Adicionar suporte a `Retry-After` header em retry de LLM
- [x] Implementar cache de respostas LLM (opt-in por `cache: verdadeiro`, em memória)
- [x] Implementar streaming com retry (SSE, transporte/429/5xx e fallback)
- [x] Adicionar `tempo_limite` configurável por tentativa

### 4.2 RAG / Bancos Vetoriais (P2)
- [x] Implementar pruning de partições no Delta Lake (Fase 6: igualdade em partições compostas + filtro residual)
- [x] Melhorar embeddings mock (tokens + trigrams hasheados, 16 dims, normalização L2)
- [x] Adicionar suporte a Pinecone com `ensure` de namespace (`describe_index_stats`)
- [x] Validar integração com Qdrant, Weaviate, Chroma, pgvector em CI real (job vector_connectors com containers pinados)

### 4.3 Avaliação (P2)
- [x] Implementar juiz multi-cadeia estruturada (`cadeia: [...]`, voto maioria/unanimidade e veredito JSON)
- [x] Implementar amostragem estratificada (estratificar_por: com cotas proporcionais e desempate por maior resto)
- [x] Adicionar `registrar_em` com POST REST para experimento e avaliação

### 4.4 LLMOps e recuperação (P1)
- [x] Contabilidade persistente de tokens e custos (`contabilidade:` + `llm_metricas`)
- [x] Observabilidade opt-in de prompts/respostas (`observabilidade:`)
- [x] Chunking sensível a sentença, parágrafo, linha e código (`fragmentar`/`dividir_texto`)
- [x] Avaliação de recuperação em índices locais e backends externos (recall, precisão, MRR, nDCG)
- [x] Hash SHA-256 para prompts/respostas omitidos e rastreamento por `trace_id`
- [x] Contabilizar embeddings declarados com `llm:` no mesmo ledger, limites e métricas

---

## 5. Agentes

### 5.1 Ferramentas (P1)
- [x] `executar:` com corpo direto em `ferramenta` (funciona; documentado no guia-06)
- [x] Adicionar validação de entrada de ferramentas (campos obrigatórios, desconhecidos e tipagem runtime)
- [x] Implementar allowlist de ferramentas em serviços HTTP (campo ferramentas: no servico, aplicado por request)

### 5.2 Agentes (P2)
- [x] `memoria: vetorial` (Sprint 3: índice por agente, top-3, T011 em valor inválido)
- [x] Adicionar suporte a múltiplos LLMs em `equipe` (supervisor com fallback)
- [x] Implementar `max_passos` com logging detalhado de cada passo
- [x] Políticas compartilháveis de orçamento e aprovação (`politica Nome:`)
- [x] Orçamento global opcional entre agentes (`compartilhado: verdadeiro`)

---

## 6. HTTP e Serviços

### 6.1 Servidor HTTP (P1)
- [x] Implementar observabilidade completa (`/metricas` em formato Prometheus)
- [x] Implementar latências por rota em `/metricas`
- [x] Graceful shutdown (Sprint 1: SIGINT/SIGTERM drenam e encerram)
- [x] Implementar conexões persistentes (keep-alive) em Windows (`select()` loop)

### 6.2 Serviços (P1)
- [x] `meio:` (middleware) em `servico` (já existe)
- [x] Validação de `entrada:` (Sprint 1: presença+T011-era 400 + tipos escalares + defaults)
- [x] Implementar versionamento de API (prefixo `/v1/`, `/v2/`)

---

## 7. Plataforma e DevOps

### 7.1 Windows Port (P1)
- [x] Quoting `cmd.exe` (Sprint 3: `tilt_shell_quote`, `quote_test.sh`; residual % documentado)
- [x] Implementar TLS via DLL no Windows (`libssl-3-x64.dll`)
- [x] Adicionar suporte a SQLite/Postgres/MySQL no Windows via dlopen/LoadLibrary
- [x] Migrar `tests/ctest` para Windows (CTest nativo via PowerShell; suíte shell de conectores permanece em Linux/macOS)

### 7.2 Packaging (P2)
- [x] Implementar installer Windows com WIX (`.msi`) com upgrade path
- [x] Adicionar assinatura de binário para releases Linux (Sigstore/Cosign keyless; bundles `.sigstore.json` anexados)
- [x] Implementar auto-updater para o tilt CLI (`tilt-atualizar`, checksum SHA-256 e troca segura de binário/stdlib)
- [x] Criar snap extension points para integrações (banco de dados, GPU) (`database-drivers` content plug, `gpu`/hardware plugs e `TILT_DRIVER_PATH`)

### 7.3 CI/CD (P2)
- [x] Adicionar testes de interoperabilidade Spark real no CI (CTest roda `spark_test` e `spark_catalog`)
- [x] Adicionar testes de regression para conectores TLS (`tests/tls_test.sh`)
- [x] Implementar testes cross-compilation ARM64 no CI (`arm64_codegen`)
- [x] Adicionar linting de código C++ (clang-tidy, cpplint) no CI (job incremental com compile database)

---

## 8. Documentação e Exemplos

### 8.1 Documentação (P2)
- [x] Completar `guia-14-roteiro.md` com roadmap detalhado de fases restantes
- [x] Adicionar exemplos executáveis para todas as features novas em cada guia (suíte `tests/docs_test.sh`: 103 blocos Tilt validados; exemplos de Delta, RAG, agentes, treino e Kafka incluídos)
- [x] Criar guia de troubleshooting com erros comuns e soluções (`docs/guia-15-troubleshooting.md`, incluído na suíte documental)
- [x] Documentar limitações de cada conector em `guia-12-limitacoes.md`
- [x] Adicionar glossary de termos técnicos em português (`docs/glossario.md`)

### 8.2 Exemplos (P2)
- [x] Adicionar exemplo completo de ETL com Delta Lake (Sprint 2:
  `exemplos/etl_delta.tilt` — CSV→agregar→verificar→delta particionado→
  anexar→ler com pruning; hermético e idempotente; no smoke do CI)
- [x] Adicionar exemplo de RAG completo (existia `exemplos/agente.tilt` —
  validado com mock e incluído no smoke do CI)
- [x] Adicionar exemplo de agente multi-étapas com ferramentas reais (`exemplos/agente_multi_etapas.tilt`, validado no smoke CI)
- [x] Adicionar exemplo de treino de modelo com dados reais (`exemplos/treino.tilt`)
- [x] Adicionar exemplo de streaming com Kafka (tema de eventos) (`exemplos/kafka_streaming.tilt`, validado em `tests/kafka_test.sh`)

---

## 9. Performance e Escalabilidade

### 9.1 Performance (P2)
- [x] Otimizar hot path do interpretador (já usa `ValueKind` + `switch` direto; não há `std::variant`/`std::visit` no caminho de execução)
- [x] Implementar cache de tipos em `semantic/checker.cpp` para projetos grandes (memoiza expressões independentes do escopo e preserva diagnósticos)
- [x] Otimizar alocação de tensores em `Tensor` (pool/reuse) (allocator pooled thread-safe, limite de 64 MiB e teste de reuso)
- [x] Benchmark de operações de tensor (matmul, conv2d) vs NumPy/Torch (runner reproduzível com C++/NumPy/PyTorch opcional em `scripts/benchmark_tensor_ops.py`)
- [x] Implementar thread pool para operações de IO paralelas (ThreadPool reutilizável com fila protegida, limite opcional e shutdown gracioso; usado pelo servidor HTTP)

### 9.2 Escalabilidade (P3)
- [x] Implementar sharding de dados para processamento distribuído (num_shards/shard_id round-robin em RAM, CSV e Parquet; validação CTest)
- [x] Adicionar suporte a cluster mode para treinamento distribuído (filesystem compartilhado, shards automáticos, all-reduce de gradientes por passo, estados do Adam sincronizados, heartbeat/retentativas e recuperação por checkpoint; CTest com dois ranks)
- [x] Implementar query pushdown para conectores SQL (`pushdown.colunas`, `onde` parametrizado e `limite` em SQLite/Postgres/DuckDB/MySQL/ClickHouse; CTest SQLite)

---

## 10. Integração com Ecossistema

### 10.1 IDE/LSP (P2)
- [x] `goto definition` no LSP (same-file e importações explícitas locais/
  `TILT_STDLIB_PATH`; índice de workspace incremental por `rootUri`/`workspaceFolders`)
- [x] Implementar `find references` no LSP (same-file e cross-file, com
  `includeDeclaration` e ranges LSP)
- [x] `hover type` (Sprint 3: assinatura de `funcao`, campos de `tipo`, tipo
  do valor em usos de variável, tipo da expressão sob o cursor com forma de
  tensor; desconhecido cai no texto atual)
- [x] Implementar `rename symbol` no LSP (same-file e cross-file, `WorkspaceEdit`
  agrupado por URI e validação de identificador)
- [x] Validar aridade de funções importadas no checker (assinaturas locais
  cacheadas por módulo; argumentos nomeados e opcionais com diagnóstico)
- [x] Adicionar diagnostics em tempo real (on-type) no LSP (push em `didChange`, pull `textDocument/diagnostic` e cache por conteúdo)

### 10.2 Formatos (P2)
- [x] Adicionar suporte a Parquet com ZSTD compression (codec 6, dlopen de libzstd, leitura/escrita e teste pyarrow nos dois sentidos)
- [x] Implementar Parquet com encryption local e AWS KMS (chave_kms,
  GenerateDataKey AES_256/Decrypt, metadata sem plaintext e testes com mock SigV4)
- [x] Adicionar suporte a Avro para Kafka Schema Registry (envelope Confluent,
  records/arrays/maps/uniões, `avro_codificar`/`avro_decodificar`, lookup e
  registro REST e integração `escrever_kafka`/`ler_kafka`)
- [x] Implementar Delta Lake transaction log parsing completo (reconciliação
  de add/remove por path+DV, time travel/CDF e Deletion Vectors inline/on-disk
  com RoaringBitmapArray portable, CRC e pruning seguro)

---

## Sprint 1 — entregue (2026-09-15/16)

1. [x] Expandir testes dourados (1.1) — 153/153 verdes; novos: `chk-tipo-registro`
   [T033], `run-experimento-preco`, `run-experimento-prever`, `chk-tipo-padrao`,
   `chk-tipo-padrao-erro`, `chk-funcao-aridade`, `run-llm-padrao`; helper
   `tests/new_golden.sh`.
2. [x] Parser/semântica (1.2/1.3) — `campo: <Tipo> = <padrao>` parseia (era T014),
   validado em `T011`, aplicado em `formato:` (mock e real) e `entrada:` de rotas;
   `= ...` fora de `tipo` é T011; aridade de `funcao` de usuário vira T011
   (faltantes sempre; sobrantes só em `f(...)` — bare-call é guloso por desenho).
   Novos fardos: `Expr::paren_call`, `Item::default_value`,
   `SemanticChecker::check_funcao_arity`, `Interpreter::field_default`.
3. [x] Busca de hiperparâmetros (3.1) — já implementada (`run_busca` + guia-04 +
   golden `run-busca` verde); só validada, sem código novo.
4. [x] Graceful shutdown HTTP (6.1) — `SIGINT`/`SIGTERM` via flag `sig_atomic_t`
   + `SignalGuard` em `HttpServer::run`, consultada nos 10 pontos de cota dos
   3 loops (epoll serial/paralelo, select); cai no `begin_shutdown` existente
   (para de aceitar, drena em voo, fecha). Teste `servico_shutdown_test.sh`.
5. [x] Validação de entrada em serviços (6.2) — presença já existia; adicionada
   verificação de tipo escalar (`texto`/`inteiro`/`decimal` com alargamento
   inteiro→decimal/`logico`) → 400 ensinável; demais tipos passam sem
   verificação. Teste `servico_entrada_test.sh` + fixture `servico_entrada.tilt`.

Triagem concluída: os blocos de checkpoint/retomar de `docs/guia-04-ml-dl.md`
foram corrigidos junto com a regra de duplicata de `treino`/`modelo`; a
documentação fecha sem o antigo T032 duplo (`docs` 102/102).

## Priorização Sugerida (restante)

### Sprint 2 — entregue (2026-09-16)

1. [x] Expandir subconjunto VM (1.3) — `Op::GetField` (mapa/tabela, `.tamanho`,
   props de tensor, `?.`), interpolação `{{nome}}` sobre locais, `ler_csv` via
   `CallFunc`+hook; codegen fail-closed + rejeição clara; goldens com paridade.
2. [x] Pooling de conexões (2.1) — `sql_pool.*` Geneérico; pg/mysql/duckdb.
3. [x] Exemplos RAG e ETL Delta (8.2) — `etl_delta.tilt` novo + `agente.tilt`
   validado; ambos no smoke do CI.
4. [x] TILT_LLM=mock (4.1) — já implementado; verificado (15 goldens + retry)
   e doc de `formato:`-com-padrão atualizada.
5. [x] Triar T032 checkpoint/retomar — virou fix: isenção `treino`+`retomar:`
   no checker + `modelo Xor:` nos 3 blocos do guia-04; docs 102/102.

### Partições nulas em colunas particionadas (concluída)
- [x] Delta: marcador Hive `__HIVE_DEFAULT_PARTITION__` no caminho, `null` no
  `partitionValues`, reidratação e poda com `nulo`, schema nullable em append e
  preservação por checkpoints; coberto por teste com pyarrow.
- [x] Iceberg: marcador no layout, `null` tipado em manifest/summary, leitura,
  pruning e schema optional em append; validado com teste funcional e pyiceberg.

### Sprint 3 — entregue
- [x] S3.4 Hover com tipos (assinatura, campos, valor em uso, expr + forma)
- [x] Iceberg nulo-partição msg (opção b: sufixo fase-26 restaurado; 158/158)
- [x] S3.1 Cache `.tiltc` (`src/vm/bytecode_cache.*`, SHA do fonte,
  fail-closed, `TILT_VM_NOCACHE`/`TILT_VM_DEBUG`, teste `tiltc_test.sh`)
- [x] S3.2 Type widening Delta/Iceberg (`integer`→`long`, `float`→`double`,
  top-level e subcolunas struct; promoção no commit com ids estáveis;
  `short`/`byte`/`decimal(p,s)` seguem erro; `schema_widen_test.sh` com
  tabelas externas pyarrow + validação pyiceberg)
- [x] S3.3 Windows (curl quoting + config seguro): `tilt_shell_quote` único
  no compat (POSIX inalterado; Win list2cmdline-style), 3 cópias removidas,
  `-w`/`--data @` citados; `tilt_enable_vt` (cores), `in_path` com PATHEXT,
  CI Windows com `-Werror` + smoke paridade; `quote_test.sh` standalone
  (sem runner Windows). Residual documentado: pares `%...%` no cmd.
- [x] S3.5 Agente `memoria: vetorial` (índice por agente, top-3 no prompt,
  `embeddings:` opcional, teto 200 turnos, `T011` em valor inválido; goldens
  `run-agente-memoria-vetorial` + `chk-memoria`)

1. [x] JIT compiler (1.3) — backend x86-64 direto em memória, fallback para VM
2. [x] Evolução de schema Iceberg/Delta (2.2)
3. [x] Agentes com memória vetorial (5.2)
4. [x] Windows port improvements (7.1); permanece apenas a migração da suíte CTest completa
5. [x] LSP features (10.1) — goto definition, hover, signature help, completion e formatting

### Estado salvo — otimização colunar (2026-09-25)

- [x] Leitura Parquet de structs de topo diretamente em `ColumnarColumn`.
- [x] Leitura Parquet de listas de structs com offsets e campos tipados.
- [x] Seleção colunar de linhas sem materializar mapas/listas intermediários.
- [x] Ordenação tipada colunar e ordenação paralela para tabelas grandes.
- [x] Filtros simples tipados e filtros compostos com máscaras de bits.
- [x] Avaliação paralela de máscaras em tabelas grandes.
- [x] Agregação colunar paralela com fusão determinística.
- [x] Hash join com chaves binárias tipadas e cache reutilizável por tabela.
- [x] Construção paralela do índice hash para tabelas grandes, com fusão determinística.
- [x] Radix sort estável para ordenação colunar inteira crescente.
- [x] Agregações colunar `variancia` (soma de quadrados) e `distintos` (conjuntos por grupo), incluindo o caminho paralelo.
- [x] Merge join linear para joins internos e à esquerda quando ambos os lados já estão ordenados pelas chaves.
- [x] Agregações `mediana` e `quantil`, com seleção parcial e fusão no caminho paralelo.
- [x] Pushdown Parquet para comparadores e predicados compostos `e`/`ou`, incluindo filtros nested projetados.
- [x] Benchmark reproduzível de joins compostos, cobrindo merge join ordenado e hash join.
- [x] Merge join composto sem serialização de chaves durante a varredura, com comparação tipada.
- [x] Propagação de metadados de ordenação colunar para evitar nova verificação no merge join.
- [x] Cache reutilizável do índice hash no lado esquerdo para right joins.
- [x] Índice de join compacto: buckets com offsets e vetor contínuo de posições, reduzindo alocações por chave.
- [x] Métricas de cache de join (`metricas_join`): bytes, buckets, posições, hits e misses.
- [x] Limite de 64 MiB por tabela para o cache de joins, com expulsão FIFO de índices antigos.
- [x] Limite de cache configurável por tabela via `limitar_cache_join(bytes)`.
- [x] Métricas de memória colunar via `metricas_memoria()`, incluindo bytes atuais, pico e cache de joins.
- [x] Pico temporário de decodificação Parquet por row group (`pico_row_group_bytes`).
- [x] Ordenação colunar de textos dictionary encoded por ranks lexicais pré-calculados.
- [x] Radix sort estável para múltiplas colunas inteiras ascendentes, aplicado da última chave à primeira.
- [x] Merge paralelo em árvore para ordenações compostas, combinando partições em rodadas paralelas.
- [x] Redução SIMD AVX2 para soma e soma de quadrados de colunas decimais, com detecção runtime e fallback escalar.
- [x] Redução SIMD por intervalos de grupos quando a tabela está ordenada pela chave de agrupamento.
- [x] `mediana_aproximada` e `quantil_aproximado` com amostragem limitada a 4096 valores por grupo.
- [x] Pool sincronizado de objetos `ValueMap` e `ValueList` via `std::pmr::synchronized_pool_resource`.
- [x] Benchmark isolado de materialização e derivação para medir o impacto do pool (`benchmark_value_pool.py`).
- [x] CI de performance executando `bench/comparar.py` em pushes e pull requests.

`selecionar: ["struct.campo"]` e `onde: {coluna: valor}` já evitam
materialização de linhas no caminho colunar; filtros escalares também usam
min/max do footer Parquet para eliminar row groups incompatíveis. Predicados
compostos, caminhos nested, joins compostos e mediana/quantis já estão cobertos
pelos itens acima. A comparação em Parquet grande (1 milhão de linhas, 100 row
groups e gzip/snappy/zstd) mostrou que o scratch reutilizável é suficiente; um
allocator dedicado adicional foi adiado por não apresentar ganho consistente. O
benchmark `scripts/benchmark_columnar_join.py`
registrou ganho colunar de 4,34x em 20 mil linhas (uma repetição local). AMD/ROCm
continua fora do escopo salvo indicação explícita.

### Ponto de retomada — 2026-09-25

- [x] Agregações colunares `variancia`, `distintos`, `mediana` e `quantil`.
- [x] Pushdown Parquet com comparadores, predicados compostos `e`/`ou` e caminhos nested.
- [x] Merge join tipado para entradas ordenadas, sem serialização de chaves na varredura.
- [x] Cache de índices nos lados direito e esquerdo, com índice compacto por buckets e vetor contínuo.
- [x] Limite de cache por tabela (`limitar_cache_join(bytes)`) e métricas `metricas_join()`.
- [x] Métricas de memória colunar (`metricas_memoria()`), incluindo pico de row group.
- [x] Testes Release, colunares, Parquet, golden, documentação e `git diff --check` passando.
- [x] Reutilização dos buffers `columns`/`coldefs` durante a leitura Parquet por row group,
  preservando capacidade e reduzindo alocações.
- [x] Allocator de scratch Parquet por leitura, reutilizando payload e cabeçalho entre páginas e row groups.
- [x] Scratch dedicado por row group também reutiliza repetition/definition levels e índices de dictionary, reduzindo reservas por página.
- [x] Benchmark nested mede tempo, RSS e `pico_row_group_bytes` (`scripts/benchmark_parquet_nested.py`).
- [x] Smoke benchmark do scratch em 100 mil linhas/20 row groups: nested colunar 27,710 ms e 15.682 KiB RSS; linhas 76,116 ms e 114.642 KiB.
- [x] Gravador Parquet com política adaptativa de memória: folhas pequenas seguem
  em paralelo; cargas acima de 256 MiB são anexadas sequencialmente para evitar
  manter todos os corpos comprimidos simultaneamente.
- [x] Writer colunar copia folhas escalares diretamente dos vetores tipados,
  evitando materialização de um `Value` por célula; nested/mixed mantém o
  decoder geral.
- [x] Medição local em 100 mil linhas após a materialização tipada por grupo e a
  concatenação nested em lote: colunar 30,744 ms/17.856 KiB RSS contra linhas
  78,048 ms/126.300 KiB; em 1 milhão, leitura colunar 115,254 ms contra
  631,331 ms por linhas (pandas: 86,599 ms).
- [x] Materialização colunar de listas e structs nested paralelizada por row
  group, limitada a duas threads para preservar o pico de memória e mantendo a
  ordem original dos grupos.
- [x] `derivar` colunar sem `Value` temporário por coluna existente: duas
  expressões aritméticas em 1 milhão de linhas levaram 0,32 s/70.236 KiB RSS.
- [x] VM com superinstruções para operandos locais/literais, remapeamento de
  saltos e tags escalares paralelas no frame.
- [x] JIT x86-64 com chamadas para funções Tilt escalares, decimais e resultados
  decimais; o callback rejeita builtins que exigem objetos e preserva o fallback.
- [x] Codegen estático ARM64 aceita as novas superinstruções e mantém chamadas e
  decimais no subconjunto nativo.
- [x] Emissor JIT AArch64 em memória para `executar --jit` (subconjunto escalar,
  chamadas, loops, impressão e decimais via helpers nativos; fallback preservado
  para estruturas e builtins fora do subconjunto).
- [x] Planos preguiçosos para `ler_csv`, `ler_parquet` e `ler_delta`, com carga
  thread-safe no primeiro acesso e fallback colunar/nativo.
- [x] Delegação opcional de `agrupar_por` e `juntar` colunares para DuckDB via
  `TILT_ANALYTIC_ENGINE`/`TILT_DUCKDB_ANALYTICS`, recuando quando a biblioteca
  ou o subconjunto SQL não estiver disponível.
- [x] Execução paralela conservadora de atribuições independentes em pipelines
  (`paralelo: verdadeiro` ou `TILT_PIPELINE_PARALLEL=1`); dependências e saída
  textual permanecem sequenciais.
- [x] Leitura colunar CSV por faixas de bytes em paralelo e loop independente
  `saida[i] = expressao` via `TILT_LOOP_PARALLEL=1`, com commit determinístico.
- [x] Planos lazy para fontes remotas e pushdown Elasticsearch/OpenSearch
  (`_source`, `bool.filter`, `size`), mantendo pushdown SQL parametrizado.
- [x] MLOps: registry local versionado com SHA-256 e linhagem de modelo,
  código e dados; `linhagem-modelo`/`rollback-modelo`; `servir --modelo` por
  stage; MLflow publica `detalhes.json` e `lineage.json`; rotas suportam
  limites, A/B/canário ponderado e rollback automático por 5xx.
- [x] LLM/agentes: spans JSONL com `trace_id`/`span_id`, parentesco,
  duração, status e atributos exportáveis por `otel_exporter:`;
  compactação automática de memória/observações por `max_contexto:`;
  orçamento detalhado de tokens/custo e médias em `llm_metricas`.
- [x] CUDA opt-in para residência f32, GEMM em lote, backward denso,
  convolução, recorrência (RNN/LSTM/GRU), embeddings, normalização,
  max-pooling e redução; fallback CPU preservado.
- [x] Dados e adoção: `ler_csv` pode inferir tipos por amostra (inclusive
  `data_hora` com conversão de fuso), `perfil` produz estatísticas e schema,
  `inferir_schema`/`validar_schema`/`evoluir_schema` mantêm contratos JSON
  versionados; `scripts/benchmark_data_backends.py` compara agregação, filtros
  e joins com pandas, Polars e DuckDB quando instalados. A rodada de 1 milhão
  de linhas está em `benchmarks/relatorio-2026-09-28.md`.
- [x] `Value` compacto: as referências para listas, mapas, tensores, funções e
  tabelas colunares agora compartilham um único bloco de armazenamento e o
  texto usa `CompactString` com SSO de 22 bytes. `sizeof(Value)` caiu de 112
  para 56 bytes; textos curtos e escalares não alocam. Os acessos internos
  usam `*_ref()` para manter a migração explícita e segura.
- [x] Matriz atualizada CSV→groupby→Parquet: Tilt colunar 197,078 ms e pandas 187,471 ms em 1 milhão de linhas; Polars/DuckDB foram marcados como indisponíveis no ambiente, sem tratar ausência como zero.
- [x] Medição de escrita colunar em 1 milhão de linhas: `row_group: 100000`
  gerou 10 grupos em 0,28 s/35.120 KiB RSS, contra 1 grupo em 0,31 s/112.580
  KiB no mesmo processo e carga.
- [x] Comparação antes/depois em 1 milhão de linhas, 100 row groups e gzip/snappy/zstd:
  leitura, roundtrip, RSS e pico de row group em `benchmarks/parquet-large-2026-09-28.md`;
  o scratch foi mantido, mas não há evidência para um allocator dedicado adicional.
- [x] Writer particiona entradas com schema estável via `row_group:`/`grupo:` e
  emite múltiplos row groups no mesmo arquivo, ajustando offsets no footer;
  entradas com schemas locais incompatíveis recuam para um único grupo.

### Investigação do pool — 2026-09-26

- [x] Benchmark com e sem pool para materialização, join e listas, validando todos os resultados.
- [x] Medir criação e destruição com 1, 2 e 4 threads; variante sem sincronização apenas como diagnóstico de uma thread.
- [x] Usar alocação padrão nos mapas materializados em lote e registrar comparação antes/depois.
- [x] Investigar listas de 0, 2, 32 e 256 elementos com liberação local e em outra thread; adotar `std::make_shared` somente em `Value::lista`.

### Validação Parquet das políticas de alocação — 2026-09-26

- [x] Comparar antes/depois em um milhão de linhas aninhadas, 100 row groups,
  medindo tempo total e RSS em leitura e leitura+escrita, linhas e colunar.
- [x] Conferir integralmente valores e nulos dos arquivos regravados com PyArrow.
- [x] Localizar conversão de colunas inteiras em `Value` no gravador Parquet;
  registrar pico de 1262 MiB no pipeline colunar contra 84 MiB na leitura isolada.
- [x] Priorizar escrita colunar por lotes/row groups e medir seu efeito no pico RSS;
  `row_group: 100000` reduziu o pico observado nesta carga de 112.580 para
  35.120 KiB.
- [x] Reavaliar allocator dedicado por row group depois de medir o novo gravador;
  foi adiado: o ganho medido veio da escrita em lotes, sem evidência suficiente
  para justificar um allocator separado agora.

### Próxima sessão — continuidade

- [ ] Manter os artefatos atuais; não limpar `build/` nem as saídas geradas de
  `tests/golden` até confirmar quais fixtures serão preservados.
- [x] Reexecutar a suíte completa e a regressão CUDA na RTX 5050: 98/98 testes
  passaram sequencialmente em 124,44 s; `gpu_cuda` e `parquet_kms` também
  passaram. A execução paralela teve uma flutuação isolada em `pipeline_ops`,
  que passou novamente sozinho.
- [x] Repetir o benchmark de Parquet em 1 milhão de linhas com gzip, Snappy e
  Zstd, comparando leitura, escrita, RSS e pico por row group em
  `benchmarks/parquet-large-2026-09-29.md`. O scratch segue útil no caminho
  colunar; não há evidência para um allocator dedicado adicional.
- [x] Atualizar a comparação com pandas, Polars e DuckDB em 1 milhão de linhas;
  os resultados estão em `benchmarks/data-backends-2026-09-29.md`. A sessão RPC
  persistente agora mede Tilt em 0,50x–0,87x do tempo de pandas após a leitura
  CSV direta; o benchmark separa o custo de inicialização do processo.
- [x] Caminho CSV colunar simples anexa inteiros, decimais, textos e nulos
  diretamente nas colunas, removendo o vetor de `Value` temporário por linha.
  Em 1 milhão de linhas, agrupamento/filtro/junção caíram para 120/132/207 ms
  no processo novo; o roundtrip completo está no relatório de backends.
- [x] Materialização nested evita cópia escalar adicional em structs e o vetor
  temporário de células em listas de structs, mantendo o decoder geral apenas
  para níveis de repetição que exigem remontagem. Em 100 mil linhas, Parquet
  colunar caiu para 27,066 ms e 16.272 KiB de RSS; detalhes em
  `benchmarks/parquet-nested-2026-09-29.md`.
- [x] Ampliar os codecs Parquet para Brotli (4) e LZ4_RAW (7), carregados via
  `dlopen` quando as bibliotecas opcionais existem; gzip, Snappy e Zstd seguem
  funcionando sem mudanças de instalação.
- [ ] Priorizar os próximos ganhos de performance: caminho simples de leitura,
  `derivar` vetorizado e materialização nested sem decodificação desnecessária.
- [ ] Manter AMD/ROCm fora do escopo; preservar fallback CPU para todos os
  caminhos CUDA/Metal.
