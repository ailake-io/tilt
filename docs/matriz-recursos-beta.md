# Matriz de recursos — `v0.2.0-beta.1`

| Área | Estado | Fallback/execução | Limite conhecido |
|---|---|---|---|
| Linguagem, checker e VM | Suportado | Interpretador, VM e JIT | Inferência entre módulos ainda é conservadora |
| CSV e JSON | Suportado | CPU, leitura lazy quando possível | Inferência de schema é por amostragem |
| Parquet | Suportado | CPU; filtros e projeções podem usar metadados | Nested muito profundo e alguns tipos lógicos ficam no subconjunto documentado |
| Delta Lake | Suportado | Catálogo local e fallback residual | Escrita concorrente é single-writer por diretório |
| Iceberg Hadoop | Suportado | Arquivos locais | REST é um subconjunto; escrita depende de metadata local materializado |
| Iceberg REST | Parcial | Leitura/commit do subconjunto implementado | Operações REST fora desse subconjunto retornam `501` |
| SQL/DuckDB | Suportado | DuckDB opcional; motor colunar local | Delegação de cargas grandes é opt-in |
| Spark/Livy | Parcial | Fallback local | Requer Livy/Spark, timeout fixo e materialização do resultado do statement |
| Agregações, joins e ordenação | Suportado | CPU multithread e spill quando habilitado | Hash join paralelo e sort externo ainda são áreas de evolução |
| CUDA | Opcional | Fallback CPU completo | Residência, backward e kernels especializados cobrem apenas o subconjunto atual |
| Metal | Opcional | Fallback CPU completo | Requer macOS/Metal; AMD/ROCm permanece fora do beta |
| ML/DL | Suportado | CPU | Operadores avançados podem voltar ao CPU |
| LLM/RAG e agentes | Suportado | `TILT_LLM=mock` para execução offline | Rede real e credenciais são fornecidas pelo ambiente |
| MLflow/registry | Suportado | Registry local sem servidor | Artefatos remotos dependem do endpoint e autenticação |
| HTTP | Suportado | Servidor local epoll/pool de rotas | TLS terminado por proxy; teste local precisa de sockets |
| Conectores externos | Parcial | Testes com mocks e fallback de erro | Kafka, bancos, S3, vetores e catálogos exigem serviço/credenciais |
| LSP | Suportado | CLI `checar`/`completar` | Integração com editor é via cliente LSP/VS Code |

“Suportado” significa que há teste automatizado no repositório. “Parcial”
significa que existe um caminho funcional, mas o contrato é deliberadamente
menor que o do sistema externo. A lista completa de limites está em
[`guia-12-limitacoes.md`](guia-12-limitacoes.md).
