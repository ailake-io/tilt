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
- [ ] Fase 12-7 — GPU real/CUDA + AMP (deferida; não bloqueia as demais fases)
- [x] Fase 12-8 — CI Windows com testes funcionais (build MSVC + CTest nativo via `tests/windows_functional.ps1`; conectores shell permanecem em Linux/macOS)
- [ ] GPU — validação em hardware CUDA real (deferida)


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
- [ ] Implementar AMP (automatic mixed precision) — roteiro
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
- [x] Implementar exportação GGUF v3 (escrita F32; quantização ainda pendente)

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
- [x] Adicionar suporte a cluster mode para treinamento distribuído (filesystem compartilhado, shards automáticos, barreira por época e média de parâmetros; CTest com dois ranks)
- [x] Implementar query pushdown para conectores SQL (`pushdown.colunas`, `onde` parametrizado e `limite` em SQLite/Postgres/DuckDB/MySQL/ClickHouse; CTest SQLite)

---

## 10. Integração com Ecossistema

### 10.1 IDE/LSP (P2)
- [x] `goto definition` no LSP (same-file; cross-file futuro)
- [ ] Implementar `find references` no LSP
- [x] `hover type` (Sprint 3: assinatura de `funcao`, campos de `tipo`, tipo
  do valor em usos de variável, tipo da expressão sob o cursor com forma de
  tensor; desconhecido cai no texto atual)
- [ ] Implementar `rename symbol` no LSP
- [ ] Adicionar diagnostics em tempo real (on-type) no LSP (hoje: full reparse
  por `didChange`, sem debounce/cache)

### 10.2 Formatos (P2)
- [ ] Adicionar suporte a Parquet com ZSTD compression
- [ ] Implementar Parquet com encryption (AWS KMS / local key)
- [ ] Adicionar suporte a Avro (para Kafka schema registry)
- [ ] Implementar Delta Lake transaction log parsing completo

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

### Enfileirado (pós-Sprint 3): aceitar nulo em coluna de partição
- Hoje: erro claro fase 26 (Delta + Iceberg). Proposta: convenção Hive
  `__HIVE_DEFAULT_PARTITION__` no layout + `null` no log/manifest, reidratar
  marcador→`nulo` na leitura + poda com `nulo`, validar pyiceberg/Spark.
  Irmão gêmeo (`/` em valores) fica de fora salvo pedido.

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
