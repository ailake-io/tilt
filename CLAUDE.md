# CLAUDE.md — Diretrizes do Projeto: Linguagem Tilt (Compilador em C++20 Nativo)

> Especificação arquitetural, sintática e técnica do compilador da linguagem **Tilt**, escrito em **C++ Moderno (C++20)**, sem LLVM e sem Rust. Sintaxe **declarativa estilo YAML**, fácil de ler e escrever, com construções de **primeira classe para Machine Learning, Deep Learning, Engenharia de Dados, LLMs e Agentes de IA**.

---

## 1. Visão Geral da Tilt

A **Tilt** existe para que uma pessoa consiga descrever um pipeline de dados, treinar um modelo, chamar um LLM ou orquestrar agentes **no mesmo arquivo, com a mesma sintaxe simples**, e compilar tudo para um binário nativo rápido.

Pilares:

1. **Fácil primeiro.** Indentação de 2 espaços, pares `chave: valor`, listas com `-`. Sem `{ }`, sem `;`, sem `()` ruidosos. Tipos são opcionais — o compilador infere. Uma forma óbvia de fazer cada coisa.
2. **Quatro domínios nativos.** `pipeline`, `modelo`, `treino`, `experimento`, `avaliacao`, `llm`, `indice`, `ferramenta`, `agente` e `equipe` são palavras-chave da linguagem, não bibliotecas.
3. **Runtime C++ enxuto.** Sem GC tradicional. APIs rodam em *Request Arenas* de liberação instantânea. Tensores residem em memória contígua alinhada (*pinned* / *unified memory*).
4. **Aceleração nativa.** Tipos de tensor n-dimensional, operadores matriciais vetorizados (SIMD/AVX) e despacho direto de kernel GPU (CUDA / ROCm / Metal) via runtime C++, com *fallback* automático em CPU.
5. **Padrões seguros.** Segredos vêm de `env`. Erros de indentação, tipo e dimensão de tensor são detectados em tempo de compilação com mensagens que ensinam.

Um programa Tilt mínimo (rode com `tilt executar mini.tilt`):

```tilt run
pipeline ola_dados:
  passos:
    # Tabela inline (em produção: ler_csv "dados/vendas.csv").
    - tabela = [
        { regiao: "sul", valor: 30 },
        { regiao: "norte", valor: 120 }
      ]
    - resumo = tabela.agrupar_por "regiao", { total: somar "valor" }
    - escrever_parquet resumo, "vendas_por_regiao.parquet"
```

---

## 2. Princípios de Design da Linguagem

| Princípio | O que significa na prática |
|-----------|---------------------------|
| **Legibilidade acima de concisão** | O código deve ser lido em voz alta sem esforço. Palavras-chave em português, sem pontuação supérflua. |
| **Progressive disclosure** | O caso simples é curto. Opções avançadas (dispositivo, sharding, quantização) são chaves extras, nunca obrigatórias. |
| **Baterias inclusas** | Conectores (CSV/Parquet/SQL/Kafka/S3...), DataFrame, HTTP, tensores, `nn`, cliente LLM, bancos vetoriais e orquestrador de agentes no runtime; `stdlib` traz `io`, `rede` e `nn` via `importar`. |
| **Padrões seguros** | Assíncrono por padrão nas rotas. Sem alocação manual. Segredos só via `env`. Sem `panic` implícito. |
| **Tipos opcionais, inferência total** | `x = 3` já é `inteiro`. Anotações servem para contratos públicos (`tipo`, `entrada:`) e formas de tensor. |
| **Erros que ensinam** | Toda mensagem aponta a linha, mostra o esperado vs. o encontrado e sugere a correção. |
| **Uma forma óbvia** | Uma sintaxe de laço, uma de condicional, uma de função. Sem açúcar redundante. |

---

## 3. Sintaxe Essencial

### 3.1 Indentação, comentários e literais
- Indentação de **2 espaços OU 1 tab por nível** — um estilo por arquivo, definido na primeira linha indentada; misturar é erro de compilação (`T002`).
- Comentário de linha: `#`.
- Literais: `texto` (`"..."`), `inteiro` (`42`), `decimal` (`3.14`), `logico` (`verdadeiro` / `falso`), `nulo`.
- Texto multilinha e interpolação: `"""..."""` e `{{expressao}}`.

### 3.2 Variáveis e funções
```tilt run
seja taxa = 0.001          # 'seja' é opcional; 'taxa = 0.001' também vale
constante MAX_TOKENS = 4096

funcao centralizar t:
  media = t.media
  retornar t - media

pipeline f:
  passos:
    - imprimir taxa * 2                    # 0.002
    - imprimir centralizar(tensor [1, 2, 3])  # [-1, 0, 1]
```

### 3.3 Controle de fluxo
```tilt run
llm gpt:
  provedor: "anthropic"
  modelo: "claude-sonnet-5"
  chave: env "ANTHROPIC_API_KEY"

pipeline fluxo:
  passos:
    - pontuacao = 0.7
    - rotulo = "?"          # declara antes: 'se' cria sub-escopo, não vaza
    - se pontuacao >= 0.9:
        rotulo = "alta"
    senao se pontuacao >= 0.5:
        rotulo = "media"
    senao:
        rotulo = "baixa"
    - imprimir rotulo       # media
    - tabela = [{ email: "a@x" }]
    - para cada linha em tabela:
        imprimir linha.email
    - tentativas = 0
    - enquanto tentativas < 3:
        tentativas = tentativas + 1
    - tentar:
        resposta = perguntar gpt, usuario: "resuma tilt"
    capturar erro:
      registrar "falha no LLM: {{erro}}"
```

### 3.4 Módulos
```tilt run
importar rede
importar io

pipeline modulos:
  passos:
    - imprimir io.existe_arquivo "/tmp"   # verdadeiro
```

### 3.5 Coleções
```tilt run
pipeline colecoes:
  passos:
    - nomes = ["ana", "bruno", "caio"]
    - config = { epocas: 10, lote: 64 }
    - imprimir nomes[0..2]     # [ana, bruno]
    - imprimir config.epocas   # 10
```

---

## 4. Tipos de Dados Nativos

| Tipo | Descrição |
|------|-----------|
| `texto`, `inteiro`, `decimal`, `logico`, `nulo` | Escalares. |
| `lista[T]`, `mapa[K, V]` | Coleções homogêneas. |
| `opcional[T]` | `T` ou `nulo`; acesso exige `se` ou `?.`. |
| `tensor[dtype, dim...]` | N-dimensional. `dtype` ∈ `f32 f16 bf16 i32 i8`. Forma validada em compilação. |
| `tabela` | DataFrame colunar (Arrow por baixo). Operações preguiçosas até `coletar`/`escrever`. |
| `fluxo[T]` | Sequência assíncrona (streaming de tokens, linhas de Kafka, eventos SSE). |
| `registro` (via `tipo`) | Struct nomeada, alocada em arena ou memória unificada. |

```tilt run
tipo EntradaInferencia:
  texto: texto
  vetor_contexto: tensor[f32, 1536]
  temperatura: decimal

tipo MetricasProcessamento:
  latencia_ms: decimal
  tokens_usados: inteiro
  dispositivo: texto

pipeline tipos:
  passos:
    - imprimir "tipos declarados"
```

---

## 5. Engenharia de Dados

### 5.1 Fontes e conectores
```tilt check
# Declarações checam sem servidor; executar precisa do serviço real.
fonte clientes:
  tipo: postgres
  url: env "DATABASE_URL"
  consulta: "select * from clientes where ativo"

fonte eventos:
  tipo: kafka
  brokers: env "KAFKA_BROKERS"
  topico: "eventos.clique"
  formato: json
```

Conectores nativos: `postgres`, `mysql`, `sqlite`, `kafka`, `s3`, `http`, `csv`, `parquet`, `json`, `delta`.

### 5.2 Pipelines declarativos
```tilt run
fonte clientes:
  tipo: csv
  caminho: "clientes.csv"

pipeline etl_clientes:
  agenda: "0 * * * *"           # cron; ausente = sob demanda (--agendar agenda)
  ao_falhar: repetir 3, espera: "30s"

  passos:
    - cru = [{ email: "ana@x.com", valor: 10 }, { email: "ruim", valor: 5 }]
    - escrever_csv cru, "clientes.csv"   # grava para o 'ler' abaixo ler
    - bruto = ler clientes
    - limpo = bruto.filtrar linha.email contem "@"
    - com_dom = limpo.derivar { dominio: dividir(linha.email, "@")[1] }
    - agregado = com_dom.agrupar_por "dominio", {
        total: contar,
        receita: somar "valor"
      }
    - escrever_parquet agregado, "vendas_por_dominio.parquet"
```

### 5.3 Qualidade de dados
```tilt run
pipeline qualidade:
  passos:
    - agregado = [{ dominio: "x.com", total: 2, receita: 20 }]
    - verificar agregado:
        - nao_nulo: [dominio, total]
        - unico: dominio
        - intervalo: linha.receita >= 0
        ao_violar: abortar        # ou 'avisar'
```

### 5.4 Streaming
```tilt check
# 'entrada:' referencia uma 'fonte' declarada; 'janela:' só faz sentido com
# --agendar (contagem, tempo ou throttle — ver guia 03).
fonte eventos:
  tipo: kafka
  brokers: env "KAFKA_BROKERS"
  topico: "eventos.clique"
  formato: json

pipeline contagem_ao_vivo:
  entrada: eventos
  janela: "1min"
  passos:
    - por_url = linhas.agrupar_por "url", { cliques: contar }
    - imprimir tamanho por_url
```

---

## 6. Machine Learning Clássico

### 6.1 Experimentos
```tilt run
experimento prever_churn:
  dados: [
    { uso: 10, plano: "a", churn: 1 },
    { uso: 9, plano: "b", churn: 1 },
    { uso: 2, plano: "a", churn: 0 },
    { uso: 1, plano: "b", churn: 0 }
  ]
  alvo: "churn"
  atributos: [uso, plano]
  pre_processar:
    - um_de_n: [plano]
    - padronizar: [uso]
  dividir: { treino: 0.75, teste: 0.25 }
  modelo: regressao_logistica
  metricas: [acuracia, f1]
  semente: 7
```

Modelos nativos: `regressao_linear`, `regressao_logistica` (binária e
multinomial), `knn`, `kmeans`, `floresta_aleatoria`,
`gradiente_impulsionado` e `svm` (ver guia 04; `imputar:`,
`validacao_cruzada:` e hiperparâmetros `arvores:`/`profundidade:`/
`taxa:`/`custo:` suportados).

### 6.2 Uso do modelo treinado
```tilt run
experimento prever_churn:
  dados: [
    { uso: 10, plano: "a", churn: 1 },
    { uso: 9, plano: "b", churn: 1 },
    { uso: 2, plano: "a", churn: 0 },
    { uso: 1, plano: "b", churn: 0 }
  ]
  alvo: "churn"
  atributos: [uso, plano]
  pre_processar:
    - um_de_n: [plano]
  modelo: regressao_logistica
  semente: 7

tipo Pedido:
  uso: inteiro
  plano: texto

# Sob 'tilt servir', POST /prever ajusta nada (o experimento já rodou na
# subida) e responde com a probabilidade. Aqui o 'executar' só ajusta.
servico Predicao:
  rota post "/prever":
    entrada: Pedido
    passos:
      - p = experimento prever_churn.prever entrada
      - responder:
          dados:
            risco_churn: p.probabilidade
```

---

## 7. Deep Learning e Tensores na GPU

### 7.1 Definição de modelo
```tilt run
modelo Classificador:
  dispositivo: auto              # cuda:0 -> metal -> cpu, nessa ordem
  entrada: tensor[f32, 1536]
  camadas:
    - densa: 512
      ativacao: relu
    - abandono: 0.1
    - densa: 128
      ativacao: relu
    - densa: 10
    - softmax
  pesos: "modelos/classificador.pesos"   # carregados se existirem

pipeline demo:
  passos:
    # Vetor de 1536 uns: atravessa a rede e sai com 10 classes.
    - entrada = uns [1536]
    - probs = modelo Classificador.executar entrada
    - imprimir probs.forma   # [10]
```

Camadas nativas: `densa`, `conv2d`, `agrupamento_max`, `abandono`, `norma_lote`, `norma_camada`, `atencao`, `incorporacao`, `recorrente`, `residual`, além de `ativacao: relu | gelu | silu | tanh | sigmoide`.

### 7.2 Treino
```tilt run
# XOR em 4 amostras: 'dados:' inline { x: tensor 2D, y: lista }.
modelo Mini:
  entrada: tensor[f32, 2]
  camadas:
    - densa: 2
    - softmax

treino Mini:
  dados: { x: [[0, 0], [0, 1], [1, 0], [1, 1]], y: [0, 1, 1, 0] }
  perda: entropia_cruzada
  otimizador: sgd
  epocas: 3
```

O `treino` real hoje: `dados:` inline `{ x: tensor 2D|4D, y: lista }`,
`carregador` (RAM) ou `carregador ..., fluxo: verdadeiro` (CSV em blocos),
`perda: entropia_cruzada | quadratica`, `otimizador: sgd | adam`,
`taxa:`/`epocas:`/`lote:`/`semente:`, `agendador:` (cosseno|degrau),
`validacao:` + `parar_cedo:`, `checkpoint:`/`a_cada:`/`retomar:`, com
backward de `densa`, `conv2d`, `norma_lote`, `agrupamento_max` e `achatar`;
`busca` faz grade de hiperparâmetros (`criterio: perda|acuracia`). AMP,
`ao_epoca` e quantização GGUF são roteiro (ver guia 04 e guia 12).

### 7.3 Tensores explícitos (controle fino)
```tilt check
# 'tarefa' com entradas tipadas (forma checada) e corpo com tensores.
tarefa treinar_passo:
  entrada:
    lote_x: tensor[f32, 64, 1536]
    pesos: tensor[f32, 1536, 512]
  executar:
    - logits = lote_x.matmul(pesos)
    - probs = logits.softmax
    - retornar probs.forma
```

O analisador semântico valida as formas (`tensor[f32, 64, 1536] @ densa[1536, 512]` → `tensor[f32, 64, 512]`) antes de gerar código.

### 7.4 Inferência e exportação
```tilt run
modelo Mini:
  entrada: tensor[f32, 2]
  camadas:
    - densa: 2
    - softmax

pipeline pesos:
  passos:
    # Salva no formato tilt-pesos (JSON); 'pesos:' do modelo carrega de volta.
    - modelo Mini.salvar_pesos "mini.pesos"
    - modelo Mini.carregar_pesos "mini.pesos"
    - modelo Mini.exportar_onnx "mini.onnx"
    - imprimir "ok"
```

Exportação ONNX via `modelo <Nome>.exportar_onnx "modelo.onnx"` (opset 20,
sem dependências) e GGUF v3 via `modelo <Nome>.exportar_gguf "modelo.gguf"`
(só escrita, F32) — ver guia 04 e guia 12.

---

## 8. LLMs

### 8.1 Declaração do provedor
```tilt run
llm gpt:
  provedor: "anthropic"          # anthropic | openai | local | vllm
  modelo: "claude-sonnet-5"
  temperatura: 0.2
  max_tokens: 1024
  chave: env "ANTHROPIC_API_KEY"
  tempo_limite: 60               # segundos por tentativa
  tentativas: 3                  # retry em transporte/429/5xx
  teto_tokens: 0                 # 0 = sem teto (ver guia 05)
  reserva: [gpt_barato]          # fallback: outro 'llm' (ver guia 05)
```

### 8.2 Chamada e prompts
```tilt run
llm gpt:
  provedor: "anthropic"
  modelo: "claude-sonnet-5"
  chave: env "ANTHROPIC_API_KEY"

pipeline resumir:
  passos:
    # Com TILT_LLM=mock, roda offline (resposta simulada determinística).
    - resposta = perguntar gpt:
        sistema: "Você resume textos técnicos em 3 frases."
        usuario: "Resuma: tilt é uma linguagem declarativa"
    - imprimir resposta.texto
```

### 8.3 Saída estruturada (garantida pelo tipo)
```tilt run
llm gpt:
  provedor: "anthropic"
  modelo: "claude-sonnet-5"
  chave: env "ANTHROPIC_API_KEY"

tipo Resumo:
  titulo: texto
  pontos: lista[texto]
  sentimento: "positivo" | "neutro" | "negativo"

pipeline extrair:
  passos:
    - dados = perguntar gpt, formato: Resumo:
        usuario: "Extraia estrutura de:\ntilt é ótimo"
    - imprimir dados.titulo   # já validado contra 'Resumo'
```

### 8.4 Streaming
```tilt check
# Streaming de tokens (SSE) numa rota; roda sob 'tilt servir'.
llm gpt:
  provedor: "anthropic"
  modelo: "claude-sonnet-5"
  chave: env "ANTHROPIC_API_KEY"

tipo EntradaChat:
  mensagem: texto

servico Chat:
  rota post "/stream":
    entrada: EntradaChat
    passos:
      - fluxo_tokens = perguntar_em_fluxo gpt, usuario: entrada.mensagem
      - responder_em_fluxo fluxo_tokens
```

### 8.5 Embeddings
```tilt run
pipeline vetores:
  passos:
    # Com TILT_LLM=mock, embeddings determinísticos de 16 dimensões.
    - vetor = incorporar "text-embedding-3-small", "texto de exemplo"
    - imprimir vetor.forma   # [16]
```

---

## 9. RAG e Bancos Vetoriais

```tilt run
indice base_conhecimento:
  embeddings: "text-embedding-3-small"
  armazenamento: "memoria"   # qdrant | pgvector | weaviate | pinecone | chroma
  dimensao: 16               # 1536 no modelo real; 16 no mock determinístico
  metrica: cosseno

pipeline rag:
  passos:
    # inserir devolve a contagem; argumento lista exige parênteses.
    - total = base_conhecimento.inserir([{ texto: "tilt é declarativa" }])
    - imprimir total   # 1
    - trechos = base_conhecimento.buscar "tilt", top_k: 1
    - para cada h em trechos:
        imprimir h.id, h.score
```

---

## 10. Agentes de IA

### 10.1 Ferramentas
```tilt check
# 'executar:' sem '-' (corpo direto); 'clima' precisa de rede para rodar,
# então este bloco só é checado aqui (o executável está em 10.2).
indice base_conhecimento:
  embeddings: "text-embedding-3-small"
  armazenamento: "memoria"
  dimensao: 16
  metrica: cosseno

ferramenta busca_documentos:
  descricao: "Busca trechos relevantes na base de conhecimento."
  entrada:
    termo: texto
    limite: inteiro
  executar:
    vetor = incorporar "text-embedding-3-small", termo
    retornar base_conhecimento.buscar vetor, top_k: limite

ferramenta clima:
  descricao: "Clima atual de uma cidade."
  entrada:
    cidade: texto
  executar:
    retornar http_get_json "https://api.clima/v1"
```

### 10.2 Agente
```tilt run
llm gpt:
  provedor: "anthropic"
  modelo: "claude-sonnet-5"
  chave: env "ANTHROPIC_API_KEY"

ferramenta eco:
  descricao: "Repete o texto de volta."
  entrada:
    texto: texto
  executar:
    retornar texto

agente AssistenteTecnico:
  llm: gpt
  papel: "Especialista em análise preditiva e dados estruturados."
  ferramentas: [eco]
  memoria: conversa            # nenhuma | conversa | vetorial
  max_passos: 8
  ao_passo:
    - registrar { passo: passo.indice, ferramenta: passo.ferramenta }

pipeline pergunta:
  passos:
    # Com TILT_LLM=mock, o planner chama cada ferramenta uma vez, em ordem.
    - r = AssistenteTecnico.responder "resuma tilt"
    - imprimir r.texto
```

### 10.3 Multi-agente
```tilt run
llm gpt:
  provedor: "anthropic"
  modelo: "claude-sonnet-5"
  chave: env "ANTHROPIC_API_KEY"

ferramenta eco:
  descricao: "Repete o texto de volta."
  entrada:
    texto: texto
  executar:
    retornar texto

agente Pesquisador:
  llm: gpt
  papel: "Pesquisa fontes."
  ferramentas: [eco]
  memoria: conversa
  max_passos: 2

agente Escritor:
  llm: gpt
  papel: "Escreve o relatório."
  ferramentas: [eco]
  memoria: conversa
  max_passos: 2

equipe PesquisaEEscrita:
  agentes:
    - pesquisador: Pesquisador
    - escritor: Escritor
  estrategia: supervisor       # sequencial | paralelo | supervisor
  supervisor: gpt
  objetivo: "Produzir relatório técnico com fontes citadas."

pipeline relatorio:
  passos:
    - r = PesquisaEEscrita.responder "relatório sobre tilt"
    - imprimir r.texto
```

### 10.4 Exposição via serviço
```tilt check
# Rota que expõe um agente; roda sob 'tilt servir'.
llm gpt:
  provedor: "anthropic"
  modelo: "claude-sonnet-5"
  chave: env "ANTHROPIC_API_KEY"

ferramenta eco:
  descricao: "Repete o texto de volta."
  entrada:
    texto: texto
  executar:
    retornar texto

agente AssistenteTecnico:
  llm: gpt
  papel: "Atende o chat."
  ferramentas: [eco]
  memoria: conversa
  max_passos: 4

tipo EntradaChat:
  mensagem: texto

servico Agente:
  porta: 8080
  rota post "/chat":
    entrada: EntradaChat
    passos:
      - r = AssistenteTecnico.responder entrada.mensagem
      - responder:
          dados:
            texto: r.texto
            passos: r.rastro
```

---

## 11. APIs e Serviços Assíncronos

```tilt check
# 'executar' devolve tensor: a rota extrai '.argmax' (serviço real sob
# 'tilt servir'; aqui só checamos).
modelo Mini:
  entrada: tensor[f32, 2]
  camadas:
    - densa: 2
    - softmax

tipo PedidoVetor:
  vetor: tensor[f32, 2]

servico ApiPredicao:
  porta: 8080
  dispositivo: "cuda:0"
  meio:                         # middleware
    - registro_requisicoes
    - limite_taxa: { por_minuto: 120 }

  rota post "/v1/predizer":
    entrada: PedidoVetor
    passos:
      - probs = modelo Mini.executar entrada.vetor
      - responder:
          status: 200
          dados:
            classe: probs.argmax
```

Cada requisição roda numa **Request Arena** própria: alocação linear, liberação instantânea ao final da resposta. Rotas são assíncronas por padrão (epoll/kqueue).

---

## 12. Arquitetura do Compilador Autônomo em C++20

Prioridade **fácil de implementar e de evoluir**: interpretador/VM primeiro, código nativo depois.

```text
Código Fonte (.tilt / YAML-style)
              │
              ▼
   [ Lexer / Indent Engine ] ──> Leitura sem cópia (std::string_view)
              │                   Tokens: CHAVE, VALOR, INDENT, DEDENT, TRACO, NOVALINHA
              ▼
        [ Tilt Parser ] ────────> Descida recursiva; AST com std::unique_ptr + std::variant
              │
              ▼
   [ Analisador Semântico ] ────> Inferência de tipos, formas de tensor,
              │                   escopos, validação de alocação e limites de GPU
              ▼
     [ Tilt IR (bytecode) ] ────> IR de registradores, simples e serializável
              │
        ┌─────┴───────────────────────────┐
        ▼                                 ▼
[ Fase 1: Interpretador de árvore ]  [ Runtime / Dispatcher ]
[ Fase 2: VM de bytecode ]            ↳ Arenas + Unified Memory
[ Fase 3: Emissor nativo x86_64/ARM ] ↳ GPU: CUDA / ROCm / Metal via FFI (dlopen)
  ↳ opcional; gera '.s' e chama 'as'/'ld'  ↳ Conectores de dados, cliente LLM, laço de agentes
```

Decisões:
- **Sem LLVM, sem Rust.** IR e codegen são próprios.
- **Interpretador de árvore** entrega a linguagem utilizável rápido; a **VM de bytecode** dá desempenho; o **emissor nativo** é fase posterior e opcional (nenhum recurso da linguagem depende dele).
- Dependências externas: preferir `std` + APIs do SO + drivers de GPU. É permitido um conjunto **pequeno e auditado** de bibliotecas *header-only* onde escrever à mão não agrega valor e atrapalha a robustez: TLS/HTTP cliente, JSON, leitura Parquet/Arrow. Nada de Boost, nada de runtime pesado.

---

## 13. Estrutura do Repositório (CMake + C++20)

```text
tilt/
├── CMakeLists.txt
├── src/
│   ├── main.cpp                 # CLI 'tilt'
│   ├── lexer/
│   ├── parser/                  # ast.hpp, parser.hpp/cpp
│   ├── semantic/                # checker.cpp (T011/T012/T030...), type.cpp
│   ├── ir/                      # bytecode do interpretador/VM
│   ├── interp/
│   │   └── interpreter.cpp      # tree-walking (pipeline, treino, experimento,
│   │                              agentes, servir) + hpp
│   ├── vm/                      # compiler.cpp (pipeline->bytecode), vm.cpp
│   ├── codegen/                 # codegen_x86_64 / codegen_arm64 (ELF)
│   ├── lsp/                     # completion, lsp_server + checar_tilt
│   ├── cli/                     # cli.cpp (checar/executar/servir/compilar...)
│   └── runtime/                 # tensor, conectores (postgres/mysql/sqlite/
│       ...                      # duckdb/clickhouse/kafka/s3/mongo/redis),
│                                # llm.cpp, http_server.cpp, gpu_runtime.cpp...
├── stdlib/
│   ├── io.tilt
│   ├── rede.tilt
│   └── nn.tilt
├── docs/                        # guias 01-14 (exemplos executáveis verificados)
├── testes/
└── exemplos/
```

---

## 14. Roteiro de Implementação Passo a Passo

> **Status (2026-09):** fases 1–5 concluídas na 1ª passada; fases 6–11 entregues
> (pruning/partição composta Delta+Iceberg, Parquet snappy/V2/listas, codecs
> Avro, enriquecimento de Mongo/Postgres/Redis/Kafka/S3, conv2d/norma_lote,
> shape solver e inferência de tipos, LSP completo, stdlib + `importar`
> funcional, codegen ARM64, HTTP genérico, conectores DuckDB/MySQL/ClickHouse/
> Elasticsearch e vetoriais Weaviate/Pinecone/Chroma). Marco 3
> (SQL com `?`, `transacao`, `consultar_sql`, MySQL prepared) e Marco 4
> (`experimento` executável: linear/logística/knn/kmeans + `prever`) também
> entregues. Os checkboxes abaixo são o plano original; o estado corrente
> por área está em `docs/guia-12-limitacoes.md`.

### Fase 1 — Lexer baseado em linhas e indentação
- [ ] Leitura de buffer com `std::string_view` (zero alocação dinâmica no lexer).
- [ ] Máquina de estados de indentação: contar espaços no início da linha (múltiplos exatos de 2), comparar com `std::vector<size_t> indent_stack`, emitir `TOKEN_INDENT` / `TOKEN_DEDENT` / `TOKEN_NEWLINE`.
- [x] Tabs na indentação → 1 tab = 1 nível, um estilo por arquivo (definido na 1ª linha indentada); mistura tab/espaços é `T002` com mensagem que ensina. *(atualizado: tabs são alternativa válida, não erro absoluto)* Reconhecer `chave:`, literais (texto, decimal, inteiro, `verdadeiro`/`falso`/`nulo`), `-`, `{ }` de mapa inline, `"""` multilinha, `{{ }}` de interpolação.

### Fase 2 — Parser dos blocos declarativos
- [ ] `ProgramNode` com declarações de alto nível.
- [x] `tipo` → registro estruturado (uniões literais `"a" | "b"`; valores
  padrão `campo: <tipo> = <valor>` parseiam, validados em `T011` e aplicados
  em `formato:`/`entrada:`).
- [ ] `fonte`, `pipeline`, `modelo`, `treino`, `experimento`, `llm`, `indice`, `fluxo`, `ferramenta`, `agente`, `equipe`, `servico` → nós dedicados com metadados de execução.
- [ ] `passos:` → lista de instruções sequenciais; encadeamento `.metodo` e chamadas sem parênteses (`ler clientes`, `perguntar gpt, usuario: x`).
- [ ] Controle de fluxo: `se/senao`, `para cada`, `enquanto`, `tentar/capturar`, `retornar`, `funcao`.

### Fase 3 — Analisador semântico e validação de tensores
- [x] Inferência de tipos ascendente; anotações opcionais viram contratos verificados. *(fase 8: subconjunto conservador — operadores, ~55 builtins, métodos, retorno de `funcao`; `T011`)*
- [~] `shape_solver`: propagar formas por `densa`, `conv2d`, `atencao`, `@` (matmul); rejeitar incompatibilidades antes do codegen. *(fase 8: formas literais com `matmul`/`conv2d`/`reformar`/`transposta`/ativações; `atencao` e formas dinâmicas ficam para runtime; `T012`)*
- [ ] Classificar alocação: escalares/registros → Request Arena; tensores → Unified Memory / ponteiro de device.
- [ ] Validar `dispositivo: auto` resolvendo a ordem `cuda → metal → cpu` em tempo de execução, mas checando o caminho em compilação.
- [ ] Verificar que segredos usam `env` e nunca literais.

### Fase 4 — IR + interpretador de árvore (linguagem já utilizável)
- [ ] `ir_builder` gera bytecode de registradores a partir da AST.
- [ ] `tree_interp` executa direto a AST para `tilt executar` e `tilt checar`.
- [ ] Integrar runtime: `pipeline` chama `data/pipeline.cpp`; `perguntar` chama `llm/client.cpp`; `agente.responder` chama `agent/loop.cpp`.

### Fase 5 — Runtime C++ e camada nativa de GPU
- [ ] Alocador `TiltArena`:
  ```cpp
  struct TiltArena {
      char*  buffer;
      size_t capacidade;
      size_t offset;
      void*  alocar(size_t tamanho, size_t alinhamento);
      void   resetar() { offset = 0; }
  };
  ```
- [ ] `Tensor` com `shape`, `stride`, ponteiro host/device e flag de dispositivo.
- [ ] Kernels: `matmul` (GEMM), `conv2d`, `relu/gelu`, soma vetorial — CUDA + fallback CPU com threads nativas e AVX.
- [ ] Bindings de GPU via `dlopen`/`LoadLibrary` (`libcuda.so`, `nvcuda.dll`, ROCm, Metal) — sem exigir o SDK CUDA na compilação do compilador.
- [ ] `autograd.cpp`: grafo reverso para gerar o laço de `treino` automaticamente.
- [ ] `net_server.cpp`: aceitação assíncrona (epoll/kqueue), uma arena por requisição.
- [ ] Conectores: Postgres (protocolo wire), Kafka, S3 (HTTP+assinatura), CSV/Parquet.
- [ ] Cliente LLM: HTTP+TLS, streaming SSE, `structured.cpp` derivando JSON Schema do `tipo`.
- [ ] Laço de agente: ciclo pensar→chamar ferramenta→observar, `max_passos`, memória plugável.

### Fase 6 — VM de bytecode (desempenho)
- [ ] Loop de despacho da `vm.cpp` sobre o bytecode serializado.
- [x] Cache de bytecode em disco (`.tiltc` — `src/vm/bytecode_cache.*`, chave
  SHA-256 do fonte, fail-closed; teste `tiltc_test.sh`).

### Fase 7 — Emissor nativo (opcional)
- [ ] `codegen_x86_64` / `codegen_arm64`: emitir `.s`, chamadas `extern "C"` para o runtime (`tilt_http_listen`, `tilt_gpu_alloc`, `tilt_tensor_matmul`, `tilt_llm_chamar`, `tilt_pipeline_rodar`).
- [ ] Invocar `as` / `ld` do SO para o binário final.

---

## 15. Comandos de Compilação e Execução

### Compilar o compilador Tilt (host C++20)
```bash
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
cmake --build . --parallel
# binário em ./bin/tilt
```

### Usar o compilador
```bash
# Checar sintaxe, indentação e formas de tensor
./bin/tilt checar exemplos/api_predicao.tilt

# Rodar direto (interpretador)
./bin/tilt executar exemplos/etl_clientes.tilt

# Compilar para binário nativo
./bin/tilt compilar exemplos/api_predicao.tilt --saida ./servidor_ia

# Executar com GPU
./servidor_ia --dispositivo cuda:0

# Rodar um pipeline agendado como serviço
./bin/tilt executar exemplos/etl_clientes.tilt --agendar
```

---

## 16. Diretrizes Técnicas de Desenvolvimento

1. **Dependências mínimas e auditadas.** `std` C++20, APIs do SO (POSIX/Win32), drivers de GPU. Bibliotecas *header-only* apenas para TLS/HTTP cliente, JSON e Parquet/Arrow. Sem Boost, sem LLVM, sem runtime de terceiros.
2. **Memória sem vazamentos.** Nós da AST em `std::unique_ptr` ou pool unificado com desalocação instantânea. Dados de requisição em `TiltArena`. Tensores em memória alinhada a 64 bytes (AVX-512, DMA host↔device).
3. **Design orientado a dados.** Tensores contíguos e alinhados; evitar `virtual` em caminho quente; preferir `std::variant` + `std::visit` a hierarquias de classe.
4. **Erros que ensinam.** Todo diagnóstico traz: caminho e linha, trecho da fonte, "esperado X, encontrado Y" e sugestão. Nunca abortar sem contexto.
5. **Assíncrono por padrão.** Rotas e conectores de streaming não bloqueiam a thread. Uma arena por requisição, resetada ao responder.
6. **Segurança.** Segredos só via `env`. Sem execução de shell implícita. Chamadas HTTP de `ferramenta` passam por allowlist opcional do `servico`.
7. **Testes.** Cada fase tem testes de ouro (`entrada.tilt` → `esperado.txt`) para lexer, parser, inferência de formas e execução.

---

## 17. Referência Rápida de Palavras-chave

| Categoria | Palavras-chave |
|-----------|----------------|
| Estrutura | `tipo` `funcao` `seja` `constante` `importar` `de` `como` `retornar` |
| Fluxo | `se` `senao` `para cada` `em` `enquanto` `tentar` `capturar` |
| Dados | `fonte` `pipeline` `verificar` `ler` `escrever` `filtrar` `derivar` `agrupar_por` `agenda` `janela` |
| ML | `experimento` `modelo` `treino` `atributos` `alvo` `dividir` `metricas` `prever` |
| DL | `camadas` `densa` `conv2d` `agrupamento_max` `achatar` `abandono` `ativacao` `perda` `otimizador` `epocas` `lote` `semente` `agendador` `validacao` `parar_cedo` `checkpoint` `retomar` `busca` `grade` `criterio` `dispositivo` `tensor` `no dispositivo` `retropropagar` |
| LLM | `llm` `perguntar` `perguntar_em_fluxo` `sistema` `usuario` `formato` `incorporar` `avaliacao` `caso` `limiar` |
| RAG | `indice` `embeddings` `armazenamento` `buscar` `inserir` `dividir_texto` |
| Agentes | `ferramenta` `descricao` `executar` `agente` `papel` `ferramentas` `memoria` `max_passos` `equipe` `estrategia` `supervisor` |
| Serviços | `servico` `porta` `rota` `entrada` `passos` `responder` `responder_em_fluxo` `meio` |
| Literais | `verdadeiro` `falso` `nulo` |

---

## 18. Estilo de Mensagens de Erro (obrigatório)

```
erro[T012]: forma de tensor incompatível
  --> treino_tensores.tilt:14:18
   |
14 |     - predicoes = modelo Classificador.para_frente lote_x
   |                   ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
   = esperado entrada tensor[f32, _, 1536], encontrado tensor[f32, 64, 768]
   = a primeira camada 'densa' foi declarada como [1536, 512]
   = sugestão: ajuste 'lote_x' para 1536 colunas ou a camada para [768, 512]
```

Regras: código estável (`T###`), local exato, trecho da fonte, esperado vs. encontrado, causa e sugestão acionável. Sem stack trace de C++ vazando para o usuário.
