# Guia 15 — Troubleshooting

Diagnóstico rápido para problemas comuns. Separe falhas de sintaxe
(`tilt checar`), falhas de runtime (`tilt executar`) e dependências externas.

## Diagnóstico mínimo

```bash
tilt --version
tilt checar programa.tilt
tilt checar --json programa.tilt
NO_COLOR=1 tilt executar programa.tilt
```

Use o caminho absoluto do binário quando o comando roda a partir de um
diretório temporário:

```bash
realpath build/release/bin/tilt
```

A checagem estática não conecta em bancos, Kafka, Livy ou LLMs. Portanto,
`tilt checar` pode passar enquanto `tilt executar` falha por indisponibilidade
externa.

## Bibliotecas dinâmicas

SQLite, PostgreSQL, DuckDB, MariaDB/MySQL, zlib, OpenSSL e CUDA são carregados
sob demanda. Verifique a arquitetura e o loader do sistema:

```bash
file "$(command -v tilt)"
ldconfig -p | rg 'libsqlite3|libpq|libduckdb|libmariadb|libmysqlclient|libssl|libz'
```

Para uma instalação portátil ou um content snap, use um diretório próprio:

```bash
export TILT_DRIVER_PATH="$PWD/drivers"
tilt executar consulta.tilt
```

O Tilt tenta o nome normal do sistema e depois cada diretório de
`TILT_DRIVER_PATH` (`:` em POSIX, `;` no Windows). Nomes com `/` ou `\\`
são tratados como caminhos explícitos.

### MySQL: `component_reference_cache.so`

Esse nome normalmente pertence a um componente/plugin do **servidor MySQL**,
não à biblioteca cliente carregada pelo Tilt (`libmariadb.so.3` ou
`libmysqlclient.so`). Não crie um symlink arbitrário.

- Se o erro aparece ao iniciar o servidor, corrija `plugin_dir`/`component_dir`
  e verifique os logs do `mysqld`.
- Se aparece no Tilt, confirme o cliente e use uma URL `mysql://` ou
  `mariadb://`; depois teste `TILT_DRIVER_PATH` com o diretório da biblioteca.
- O diagnóstico de `dlopen`/símbolo ausente não significa que o Tilt precise do
  plugin interno do servidor.

## Conectores e TLS

Confirme URL, arquivo e credenciais antes de investigar o parser. Para SQLite,
o caminho após `sqlite://` é local; para Postgres/MySQL, o servidor precisa
aceitar a conexão.

```bash
tilt executar tests/fixtures/sqlite_params.tilt
TILT_SQL_POOL=0 tilt executar consulta.tilt
```

`TILT_SQL_POOL=0` isola problemas de reuso de conexão, mas não corrige
bibliotecas ausentes ou autenticação. Em diagnóstico TLS,
`TILT_TLS_SKIP_VERIFY=1` separa certificado de transporte; não use em produção.

## Kafka

```bash
export KAFKA_BOOTSTRAP=127.0.0.1:9092
tilt executar exemplos/kafka_streaming.tilt
```

Com `grupo:`, o offset é commitado no broker. Uma segunda execução pode vir
vazia por desenho; use outro grupo para reprocessar o histórico. Sem grupo,
`desde: "inicio"` lê o histórico e `desde: "fim"` aguarda mensagens novas.

## Spark, Iceberg e Delta

- Livy: confirme `url`, `lingua` e que a sessão Spark chegou a `idle`.
- Iceberg REST: use `ICEBERG_CATALOG=rest` e `ICEBERG_URI`; sem essas variáveis,
  o modo Hadoop local continua ativo.
- Delta/Iceberg: verifique permissões e writers concorrentes; a implementação é
  single-writer.

## LLM, RAG e agentes

Para separar a linguagem da rede, rode com o mock:

```bash
TILT_LLM=mock tilt executar exemplos/agente_multi_etapas.tilt
```

Sem o mock, confira chave, modelo, endpoint e timeout. Índices em memória não
exigem serviço; Qdrant, Weaviate, Chroma, Pinecone e pgvector exigem servidor,
URL e credencial compatíveis.

## Snap e GPU

Confirme os plugs com `snap connections tilt`. Drivers externos usam o content
id `tilt-database-drivers-v1`; o plug `gpu` fornece OpenGL e observação de
hardware. A validação CUDA em hardware real está deferida.

```bash
snap connections tilt
sudo snap connect tilt:database-drivers provider:tilt-database-drivers
sudo snap connect tilt:gpu
```

Nesta máquina, `TILT_GPU=fake` serve apenas para exercitar dispatch e mensagens,
não para validar desempenho ou compatibilidade CUDA.

## Windows e editores

No Windows, use `tilt.exe`, PowerShell e o CTest nativo. Bibliotecas são
carregadas por `LoadLibrary`; os nomes esperados incluem `libpq.dll`,
`sqlite3.dll`, `libmariadb.dll` e `nvcuda.dll`.

Para o LSP, confirme que o editor aponta para o mesmo binário do terminal e que
`tilt lsp` recebe JSON-RPC por stdin. Reinicie o processo do editor antes de
investigar cache de realce.

Consulte também o [glossário técnico](glossario.md) para os termos usados nos
diagnósticos e nos demais guias.
