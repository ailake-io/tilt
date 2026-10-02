# Exemplos reproduzíveis da beta

Os exemplos abaixo não precisam de dados secretos para o caminho local. Rode
`tilt checar` antes de executar e use o binário gerado pelo preset Release.

## Dados: CSV, filtro e agregação

```bash
tilt checar exemplos/resumo_vendas.tilt
tilt executar exemplos/resumo_vendas.tilt
```

O exemplo lê `exemplos/dados/vendas.csv`, filtra `valor >= 50` e agrupa por
região. Para testar Parquet, troque a fonte por `ler_parquet` e escolha
`codec: "zstd"`, `"snappy"` ou `"gzip"`.

## ML/DL em CPU

```bash
tilt checar exemplos/inferencia.tilt
tilt executar exemplos/inferencia.tilt
tilt executar exemplos/treino.tilt
```

`dispositivo: auto` usa CUDA/Metal quando o runtime consegue inicializá-los e
volta para CPU sem mudar o programa. Para forçar CPU, use `TILT_GPU=off`.

## LLM/RAG offline

```bash
TILT_LLM=mock tilt checar exemplos/rag_llm.tilt
TILT_LLM=mock tilt executar exemplos/rag_llm.tilt
TILT_LLM=mock tilt executar exemplos/agente.tilt
```

O mock é determinístico e não envia dados à rede. Para um provedor real,
forneça a chave pelo ambiente e remova `TILT_LLM=mock`.

## Serviço HTTP

```bash
tilt checar exemplos/servico.tilt
tilt servir exemplos/servico.tilt --porta 8080 --threads 2
curl -s http://127.0.0.1:8080/saude
curl -s -X POST http://127.0.0.1:8080/pedidos \
  -H 'content-type: application/json' \
  -d '{"cliente":"Ana","valor":100}'
```

O servidor exige permissão para escutar em `127.0.0.1`; em ambientes
restritos, execute esse exemplo em uma máquina/container que permita sockets.
