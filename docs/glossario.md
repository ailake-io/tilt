# Glossário técnico

Termos usados na linguagem Tilt, no runtime e nos guias.

| Termo | Significado |
|---|---|
| AST | Árvore sintática abstrata: representação estruturada do programa depois do parser. |
| Avro | Formato de dados com schema usado pelo Iceberg para manifests e metadata. |
| Backfill | Reprocessamento de dados anteriores a partir de um cursor ou watermark. |
| Checkpoint | Estado persistido que permite retomar uma janela, grupo Kafka ou treino. |
| Consumer group | Grupo Kafka que divide partições e commita offsets para continuar o consumo. |
| Content interface | Interface Snap para montar arquivos fornecidos por outro snap em um diretório do consumidor. |
| Delta Lake | Formato de tabela baseado em Parquet e transaction log, usado pela Tilt para append, pruning e evolução limitada. |
| dlopen | API POSIX para carregar uma biblioteca compartilhada em runtime; no Windows o equivalente é LoadLibrary. |
| Field ID | Identificador estável de coluna no schema Iceberg, independente da posição ou do nome histórico. |
| Iceberg | Formato de tabela analítica com snapshots, manifests, schemas versionados e partition specs. |
| JIT | Just-in-time compilation: compilação do bytecode para código nativo durante a execução. |
| LLM | Large Language Model: modelo de linguagem usado por perguntar, agentes e ferramentas. |
| Livy | Servidor HTTP que cria sessões e executa Spark remotamente. |
| LSP | Language Server Protocol: protocolo usado por editores para diagnóstico, hover, definição e completions. |
| Manifest | Arquivo Iceberg que lista data files e seus metadados de partição/bounds. |
| Parquet | Formato colunar com schemas, row groups, páginas e codecs de compressão. |
| Partition pruning | Poda de arquivos ou partições antes da leitura, usando predicados e summaries. |
| Row group | Grupo independente de linhas em um arquivo Parquet, com metadados por coluna. |
| Safetensors | Formato seguro e simples para armazenar tensores nomeados sem execução de código. |
| Schema evolution | Alteração compatível de schema, como adicionar coluna nullable mantendo IDs antigos. |
| Snap | Pacote Linux autocontido; na Tilt, pode receber drivers externos por content plugs. |
| Tensor | Estrutura numérica densa com forma (shape) e dados F32 no caminho CPU atual. |
| Tagged union | Representação com um discriminador (ValueKind) indicando qual variante de valor está ativa. |
| VM | Máquina virtual que executa o bytecode Tilt; pode receber fallback para o interpretador. |
| Watermark | Marca de progresso de uma fonte ou janela usada para evitar reprocessamento indevido. |

## Termos de ML e IA

| Termo | Significado |
|---|---|
| AMP | Automatic mixed precision: uso combinado de precisões numéricas; CUDA/AMP real está deferido. |
| BPTT | Backpropagation through time, retropropagação usada no treino de RNN/LSTM/GRU. |
| Embedding | Vetor numérico que representa texto, categoria ou índice em um espaço contínuo. |
| GGUF | Formato de pesos e metadata voltado a modelos, atualmente exportado pela Tilt em F32. |
| ONNX | Formato interoperável para grafos e pesos de modelos. |
| RAG | Retrieval-Augmented Generation: recuperação de documentos/vetores antes da geração. |
| RNN/LSTM/GRU | Camadas recorrentes para sequências; a Tilt possui caminho CPU com BPTT. |
| Safetensors | Formato nativo recomendado para pesos de produção; ONNX é o formato de interoperabilidade. |
