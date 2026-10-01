# Validação da beta — 1º de outubro de 2026

## Suíte local

Executada fora do sandbox, com permissão para abrir sockets locais:

```bash
ctest --test-dir build/release --repeat until-pass:2 --output-on-failure
```

Resultado: **107/107 testes aprovados (100%)**, em 135,49 s. A execução
incluiu CUDA fake/real quando disponível, Metal stub, Parquet/KMS, Delta,
Iceberg, Spark/Livy, DuckDB, MLflow, LLM, HTTP, LSP, JIT e fuzz do frontend.

Revalidação fora do sandbox em 01/10/2026, com `-j2`, repetiu o resultado em
**69,44 s**. Uma execução isolada encontrou apenas um certificado MariaDB ainda
não válido durante a inicialização; a repetição do teste `mysql` e a suíte
completa passaram sem alteração no runtime.

## Classificação de dependências externas

| Grupo | Testes/recursos | Como a suíte valida |
|---|---|---|
| Núcleo determinístico | linguagem, VM, coluna, tensor, Parquet local, Delta, Iceberg Hadoop, LSP | sem rede; executa em qualquer máquina compatível |
| Serviços simulados locais | HTTP, RPC, LLM, MLflow, REST catalog, Kafka, Redis, Mongo, S3, vetores | servidores/mocks efêmeros em `127.0.0.1`; exigem permissão de socket |
| Bibliotecas opcionais | CUDA, DuckDB, OpenSSL, libcurl, codecs | detectadas no ambiente; o teste usa fallback ou stub quando ausentes |
| Serviços reais | Spark/Livy, bancos e provedores cloud | smoke tests condicionais; credenciais e endpoints vêm do ambiente |

Os testes que abrem sockets locais são `llm_robusto`, `es_https`, `rpc_http`,
`agent_native`, `servico_ops`, `servico_entrada`, `servico_allowlist`,
`servico_versao`, `servico_shutdown`, `http`, `par`, `s3`, `http_client`,
`mlops_rollout`, `kafka`, `kafka_txn`, `redis`, `mongo`, `mongo_aggr`,
`iceberg_rest`, `iceberg_catalog`, `livy`, `clickhouse`, `elasticsearch`,
`checkpoint_backend`, `tls`, `weaviate`, `pinecone` e `chroma`. Os testes
`redis_real`, `mongo_real`, `pg`, `mysql`, `vector_real` e `spark` também podem
ser condicionais conforme as bibliotecas/serviços disponíveis, mas o runner
atual os executou sem falha.

No sandbox restrito, os 31 testes de serviço falharam exclusivamente com
`PermissionError: Operation not permitted` ao criar/listar sockets. A mesma
suíte fora do sandbox passou integralmente. Isso separa limitação do ambiente
de regressão do runtime; em CI/release o job deve manter sockets locais
habilitados e classificar indisponibilidade de um serviço real como `skip`.

Uma execução isolada apresentou timeout transitório em `llm_robusto`; a
repetição imediata passou. O comando de release usa `--repeat until-pass:2`
para capturar esse tipo de flutuação sem mascarar falhas persistentes.

O smoke manual do Spark/Livy também foi executado no container `tilt-livy`
(`openeuler/livy:0.8.0-oe2403sp1`), confirmando que filtro composto, projeção
e limite chegam ao SQL remoto antes da materialização.
