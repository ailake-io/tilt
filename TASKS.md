# Tarefas do Projeto Tilt

> Gerado em 2026-09-15 com base no estado do repositório.

## Legenda
- 🔴 **P0** — Crítico / Bloqueante
- 🟠 **P1** — Importante / Alto valor
- 🟡 **P2** — Recomendado / Melhoria
- 🟢 **P3** — Desejável / Nice to have

---

## 1. Linguagem e Compilador

### 1.1 Infraestrutura de Testes (P1)
- [x] Expandir cobertura de testes dourados em `tests/golden/` para todas as novas features (149 casos verdes; novos: `chk-tipo-registro` [T033], `run-experimento-preco`, `run-experimento-prever`; helper `tests/new_golden.sh`)
- [x] Adicionar testes de regressão para bugs corrigidos (ex: T011, T012)
- [ ] Criar teste automatizado de compatibilidade entre interpretador e VM (`tests/native_test.sh`, `tests/native_arm64_test.sh`)
- [ ] Integrar testes de lint/estilo no CI (`.github/workflows/ci.yml`)

### 1.2 Parser e Semântica (P1)
- [ ] Completar parse de `tipo` com valores padrão (`campo: <tipo> = <valor>`)
- [ ] Validar aridade de funções de usuário (atualmente sem validação)
- [ ] Implementar inferência de tipos para fluxo condicional (T011 — subconjunto conservador já existe)
- [ ] Completar solver de formas para `atencao` dinâmica e `conv2d` com formas dinâmicas (T012)

### 1.3 VM e Codegen (P1)
- [x] Expandir subconjunto VM para cobrir mais builtins (`ler_csv`, interpolação, membros)
  (Sprint 2: `Op::GetField` com paridade total — mapa/tabela, `.tamanho`, props
  de tensor, `?.` — interpolação `{{nome}}` sobre locais via `+`, `ler_csv`
  via `CallFunc`+hook; codegen fail-closed com rejeição clara; goldens
  `run-vm-interpola/membro/ler-csv` com paridade interp×VM verificada)
- [ ] Implementar JIT (compilação em runtime sem passar por `.s`+`cc`)
- [ ] Adicionar testes end-to-end para codegen ARM64 em qemu (atualmente depende de toolchain cross)
- [ ] Implementar cache de bytecode em disco (`.tiltc`) — Fase 6 da roadmap

---

## 2. Runtime e Conectores

### 2.1 Conectores de Dados (P1)
- [ ] Adicionar connector `delta` nativo (não listado em `CMakeLists.txt` como `delta.cpp` — apenas via parquet)
- [ ] Validar conectores PostgreSQL, MySQL, DuckDB, ClickHouse em CI real (apenas smoke test no Windows)
- [ ] Melhorar tratamento de erros em conectores TLS (redis/mongo/kafka) — certificados auto-assinados
- [x] Adicionar pooling de conexões para bancos relacionais (Sprint 2:
  `src/runtime/sql_pool.*` genérico por (backend, url) — postgres/mysql/duckdb
  nos statements avulsos, `transacao` dedicada, BEGIN/START/SET descartam,
  `TILT_SQL_POOL*` envs; sqlite/clickhouse fora com motivo; teste
  `sql_pool_test.sh` + driver standalone sem servidor)

### 2.2 Parquet/Delta/Iceberg (P2)
- [ ] Suportar 3+ níveis de lista no Parquet (atualmente limitado)
- [ ] Suportar structs com `field_ids` explícitos (caminho Iceberg)
- [ ] Implementar evolução de schema para Delta Lake (fase 27)
- [ ] Implementar evolução de schema para Iceberg (fase 27)
- [ ] Implementar REST catalog do Iceberg (fase 29)
- [ ] Implementar `tilt servir-catalogo` (fase 30)
- [ ] Adicionar deletes (position/equality) para Iceberg (fase 12-5a — parcial)

### 2.3 Streaming (P2)
- [ ] Implementar streaming de Parquet no treino (`carregador ..., fluxo: verdadeiro` — apenas CSV)
- [ ] Adicionar suporte a Kafka transactions multi-partição
- [ ] Adicionar persistência de estado entre disparos cron (`--agendar`)

---

## 3. Machine Learning

### 3.1 Experimentos (P1)
- [ ] Implementar `busca` de hiperparâmetros em grade (criterio: perda|acuracia, máx 64 combinações)
- [ ] Adicionar `imputar` no `pre_processar` (sintaxe `- chave: [cols]`)
- [ ] Implementar `f1` ponderado pelo suporte (atualmente limitado)
- [ ] Adicionar `validacao_cruzada` para experimentos
- [ ] Implementar `registrar_em: mlflow://` (atualmente grava JSON local)

### 3.2 Treino (P1)
- [ ] Implementar AMP (automatic mixed precision) — roteiro
- [ ] Implementar `ao_epoca` (callback por época) — roteiro
- [ ] Adicionar dataloader de Parquet para treino — roteiro
- [ ] Implementar dilation e padding explícito em conv2d
- [ ] Implementar dataloader com `shuffle` configurável

### 3.3 Modelos (P2)
- [ ] Adicionar camada `incorporacao` (embedding layer)
- [ ] Adicionar camada `recorrente` (RNN/LSTM/GRU)
- [ ] Adicionar camada `residual`
- [ ] Implementar `salvar_pesos`/`carregar_pesos` em formato ONNX
- [ ] Implementar exportação GGUF v3 (roteiro — apenas escrita F32)

---

## 4. LLM e RAG

### 4.1 LLM (P1)
- [ ] Implementar `TILT_LLM=mock` para testes determinísticos offline
- [ ] Adicionar suporte a `Retry-After` header em retry de LLM
- [ ] Implementar cache de respostas LLM — roteiro
- [ ] Implementar streaming com retry (atualmente sem retry em streaming)
- [ ] Adicionar `tempo_limite` configurável por tentativa (atualmente timeout fixo)

### 4.2 RAG / Bancos Vetoriais (P2)
- [ ] Implementar pruning de partições no Delta Lake (parcialmente funcional)
- [ ] Melhorar embeddings mock (atualmente bag-of-tokens hasheado — 16 dims)
- [ ] Adicionar suporte a Pinecone com `ensure` de namespace
- [ ] Validar integração com Qdrant, Weaviate, Chroma, pgvector em CI real

### 4.3 Avaliação (P2)
- [ ] Implementar juiz multi-cadeia estruturada (atualmente sem cadeia)
- [ ] Implementar amostragem estratificada (atualmente só por contagem)
- [ ] Adicionar `registrar_em` com POST REST (atualmente apenas JSON local)

---

## 5. Agentes

### 5.1 Ferramentas (P1)
- [ ] Implementar `execucao` com corpo direto (sem `-`) para `ferramenta` — verificar se funciona
- [ ] Adicionar validação de entrada de ferramentas (tipagem de campos)
- [ ] Implementar allowlist de ferramentas em serviços HTTP

### 5.2 Agentes (P2)
- [ ] Implementar `memoria: vetorial` (atualmente apenas `conversa` e `nenhuma`)
- [ ] Adicionar suporte a múltiplos LLMs em `equipe` (supervisor com fallback)
- [ ] Implementar `max_passos` com logging detalhado de cada passo

---

## 6. HTTP e Serviços

### 6.1 Servidor HTTP (P1)
- [ ] Implementar observabilidade completa (`/metricas` em formato Prometheus)
- [ ] Implementar latências por rota em `/metricas` (atualmente ausente)
- [ ] Adicionar graceful shutdown no servidor HTTP
- [ ] Implementar conexões persistentes (keep-alive) em Windows (`select()` loop)

### 6.2 Serviços (P1)
- [ ] Implementar middleware customizado em `servico` (além de `registro_requisicoes` e `limite_taxa`)
- [ ] Adicionar validação de entrada (`entrada:`) contra `tipo` definido em rotas
- [ ] Implementar versionamento de API (prefixo `/v1/`, `/v2/`)

---

## 7. Plataforma e DevOps

### 7.1 Windows Port (P1)
- [ ] Validar quoting de argumentos `curl` no Windows (`cmd.exe` vs PowerShell)
- [ ] Implementar TLS via DLL no Windows (`libssl-3-x64.dll`)
- [ ] Adicionar suporte a SQLite/Postgres/MySQL no Windows via dlopen/LoadLibrary
- [ ] Migrar `tests/ctest` para Windows (atualmente shell-script only)

### 7.2 Packaging (P2)
- [ ] Implementar installer Windows com WIX (`.msi`) com upgrade path
- [ ] Adicionar assinatura de binário para releases Linux
- [ ] Implementar auto-updater para o tilt CLI
- [ ] Criar snap extension points para integrações (banco de dados, GPU)

### 7.3 CI/CD (P2)
- [ ] Adicionar testes de interoperabilidade Spark real no CI (atualmente local)
- [ ] Adicionar testes de regression para conectores TLS
- [ ] Implementar testes cross-compilation ARM64 no CI
- [ ] Adicionar linting de código C++ (clang-tidy, cpplint) no CI

---

## 8. Documentação e Exemplos

### 8.1 Documentação (P2)
- [ ] Completar `guia-14-roteiro.md` com roadmap detalhado de fases restantes
- [ ] Adicionar exemplos executáveis para todas as features novas em cada guia
- [ ] Criar guia de troubleshooting com erros comuns e soluções
- [ ] Documentar limitações de cada conector (atualmente em `guia-12-limitacoes.md`)
- [ ] Adicionar glossary de termos técnicos em português

### 8.2 Exemplos (P2)
- [x] Adicionar exemplo completo de ETL com Delta Lake (Sprint 2:
  `exemplos/etl_delta.tilt` — CSV→agregar→verificar→delta particionado→
  anexar→ler com pruning; hermético e idempotente; no smoke do CI)
- [x] Adicionar exemplo de RAG completo (existia `exemplos/agente.tilt` —
  validado com mock e incluído no smoke do CI)
- [ ] Adicionar exemplo de agente multi-étapas com ferramentas reais
- [ ] Adicionar exemplo de treino de modelo com dados reais (CSV → treino → prever)
- [ ] Adicionar exemplo de streaming com Kafka (tema de eventos)

---

## 9. Performance e Escalabilidade

### 9.1 Performance (P2)
- [ ] Otimizar hot path do interpretador (reduzir overhead de `std::variant` visit)
- [ ] Implementar cache de tipos em `semantic/checker.cpp` para projetos grandes
- [ ] Otimizar alocação de tensores em `Tensor` (pool/reuse)
- [ ] Benchmark de operações de tensor (matmul, conv2d) vs NumPy/Torch
- [ ] Implementar thread pool para operações de IO paralelas

### 9.2 Escalabilidade (P3)
- [ ] Implementar sharding de dados para processamento distribuído
- [ ] Adicionar suporte a cluster mode para treinamento distribuído
- [ ] Implementar query pushdown para conectores SQL

---

## 10. Integração com Ecossistema

### 10.1 IDE/LSP (P2)
- [ ] Implementar `goto definition` no LSP
- [ ] Implementar `find references` no LSP
- [ ] Implementar `hover type` (mostrar tipo ao passar o mouse)
- [ ] Implementar `rename symbol` no LSP
- [ ] Adicionar diagnostics em tempo real (on-type) no LSP

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

Pré-existente (não é da Sprint 1, fica para triagem): 3 blocos `tilt run` em
`docs/guia-04-ml-dl.md` (checkpoint/retomar, trabalho não-commitado anterior)
falham com T032 duplo `treino Xor` — o doc escreve dois `treino Xor:` no mesmo
arquivo e só o par treino+modelo é isento de duplicata.

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

### Sprint 3 (restante)

### Sprint 3
1. JIT compiler (1.3)
2. Evolução de schema Iceberg/Delta (2.2)
3. Agentes com memória vetorial (5.2)
4. Windows port improvements (7.1)
5. LSP features (10.1)
