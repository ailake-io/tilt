# Changelog

## [0.2.0-beta.2] - 2026-10-02

Release beta com correções de estabilidade e empacotamento:

- corrige acessos inválidos detectados por UBSan na cópia de valores da VM;
- estabiliza testes de cloud provider no macOS e os mocks de conectores;
- corrige a preparação do benchmark, incluindo a dependência `pyarrow`;
- atualiza fixtures, golden tests e referências de distribuição para beta.2.

## [0.2.0-beta.1] - 2026-10-01

Primeira versão beta pública do Tilt.

- motor colunar com planos lazy, projeção tardia, filtros compostos e
  agregação fusível para CSV, Parquet e Delta;
- pushdown nativo para SQL, Elasticsearch/OpenSearch e Spark via Livy;
- Parquet com gzip, Snappy, Zstandard, LZ4 raw, Brotli opcional, listas
  nested, row groups, dictionary encoding e Parquet KMS local;
- Delta Lake e Iceberg Hadoop/REST em um subconjunto documentado, com
  evolução de schema e concorrência otimista local;
- tensores CPU com fallback completo, CUDA opcional, Metal opcional e
  residência explícita para as operações suportadas;
- treino CPU, importação GGUF/SafeTensors, exportação ONNX, registry local,
  MLflow e lineage de dados, código e modelo;
- RAG, agentes, políticas de orçamento, contabilidade de tokens/custos e
  observabilidade estruturada;
- servidor HTTP com validação, middleware, keep-alive e pool de rotas;
- VM, JIT x86-64/ARM64, compilação nativa e LSP com índice de workspace,
  referências, rename e signature help;
- pacotes CPack para Linux, Windows e macOS, com checksums e assinatura
  Sigstore no workflow de release.

Os números de desempenho publicados nesta versão são comparativos e
reproduzíveis, mas não constituem promessa de superar Polars ou DuckDB em
cargas grandes. Consulte [`benchmarks/roadmap-performance.md`](benchmarks/roadmap-performance.md)
e [`docs/guia-16-desempenho.md`](docs/guia-16-desempenho.md).

## [0.1.0]

Versão inicial do compilador/runtime e dos exemplos públicos.
