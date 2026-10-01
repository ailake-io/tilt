# Treinamentos Tilt

Esta pasta reúne exemplos pequenos e reproduzíveis das áreas principais da
linguagem. Cada área possui uma versão em português (`portugues.tilt`) e uma
versão em inglês (`english.tilt`). Os dois arquivos exercitam a mesma ideia;
os aliases de palavras reservadas são convertidos pelo lexer do Tilt.

Execute os comandos a partir da raiz do projeto:

```bash
bin/tilt checar treinamentos/01_linguagem/portugues.tilt
bin/tilt checar treinamentos/01_linguagem/english.tilt
bin/tilt executar treinamentos/01_linguagem/portugues.tilt
bin/tilt executar treinamentos/01_linguagem/english.tilt
```

Use `build/release/bin/tilt` quando estiver usando o build local. Para validar
todos os exemplos sem executar serviços externos:

```bash
for arquivo in treinamentos/*/*.tilt; do
  build/release/bin/tilt checar "$arquivo" || exit 1
done
```

## Mapa dos exemplos

| Pasta | O que demonstra | Dependência de ambiente |
|---|---|---|
| `01_linguagem` | funções, tipos inferidos, coleções, condicionais e laços | nenhuma |
| `02_dados` | CSV colunar, lazy scan, filtro, projeção, derivação e agregação | arquivo de exemplo local |
| `03_formatos` | Parquet, compressão, Delta e Iceberg locais | diretório de saída gravável |
| `04_ml` | modelo denso, carregador CSV, treino Adam e inferência | CPU; GPU é opcional |
| `05_gpu` | tensores, residência por dispositivo e fallback CPU | CUDA/Metal opcional |
| `06_llm` | LLM, RAG, embeddings, contabilidade e observabilidade | `TILT_LLM=mock` ou credenciais |
| `07_agentes` | ferramenta, agente, limite de passos e orçamento | `TILT_LLM=mock` para modo offline |
| `08_servico` | serviço HTTP, rotas, entrada tipada e resposta JSON | sockets locais para executar |
| `09_mlops` | contratos de schema e preparação para registry/linhagem | arquivo local; registry via CLI |
| `10_streaming` | fonte Kafka, janela e sobreposição | broker Kafka para executar |
| `11_sql_conectores` | consulta e DDL parametrizados | SQLite local; outros bancos são opcionais |
| `12_testes_nativo` | testes, afirmações e função compatível com codegen | nenhuma; `tilt testar` para testes |

## Execução por área

Os exemplos de dados e formatos devem ser executados a partir da raiz para que
os caminhos `exemplos/dados/...` sejam encontrados. Os exemplos que escrevem
arquivos usam `treinamentos/saida/`; crie esse diretório ou deixe o runtime
criá-lo conforme o ambiente.

Para LLM e agentes, rode primeiro:

```bash
TILT_LLM=mock build/release/bin/tilt executar treinamentos/06_llm/portugues.tilt
TILT_LLM=mock build/release/bin/tilt executar treinamentos/06_llm/english.tilt
```

Para iniciar o serviço HTTP:

```bash
build/release/bin/tilt servir treinamentos/08_servico/portugues.tilt --porta 8090
```

Para exercitar o registry local, o pipeline mostra o comando CLI equivalente:

```bash
build/release/bin/tilt registrar-modelo churn pesos.json --versao 1.0.0
build/release/bin/tilt listar-modelos
```

O exemplo GPU declara `auto`/`gpu`; quando CUDA ou Metal não estiverem
disponíveis, o runtime deve usar o caminho CPU documentado. AMD/ROCm continua
fora do escopo atual.

Os arquivos são exemplos didáticos, não uma promessa de que cada conector
externo esteja disponível localmente. Kafka, serviços HTTP, bancos remotos,
S3, Spark/Livy e provedores de LLM exigem o serviço e as credenciais indicados
na documentação correspondente.
