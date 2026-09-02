# CLAUDE.md — Diretrizes do Projeto: Linguagem Tilt (Compilador em C++20 Nativo)

> Especificação arquitetural, sintática e técnica do compilador da linguagem **Tilt**, escrito em **C++ Moderno (C++20)**, sem LLVM e sem Rust. Sintaxe **declarativa estilo YAML**, fácil de ler e escrever, com construções de **primeira classe para Machine Learning, Deep Learning, Engenharia de Dados, LLMs e Agentes de IA**.

---

## 1. Visão Geral da Tilt

A **Tilt** existe para que uma pessoa consiga descrever um pipeline de dados, treinar um modelo, chamar um LLM ou orquestrar agentes **no mesmo arquivo, com a mesma sintaxe simples**, e compilar tudo para um binário nativo rápido.

Pilares:

1. **Fácil primeiro.** Indentação de 2 espaços, pares `chave: valor`, listas com `-`. Sem `{ }`, sem `;`, sem `()` ruidosos. Tipos são opcionais — o compilador infere. Uma forma óbvia de fazer cada coisa.
2. **Quatro domínios nativos.** `pipeline`, `modelo`, `treino`, `experimento`, `llm`, `indice`, `ferramenta`, `agente` e `equipe` são palavras-chave da linguagem, não bibliotecas.
3. **Runtime C++ enxuto.** Sem GC tradicional. APIs rodam em *Request Arenas* de liberação instantânea. Tensores residem em memória contígua alinhada (*pinned* / *unified memory*).
4. **Aceleração nativa.** Tipos de tensor n-dimensional, operadores matriciais vetorizados (SIMD/AVX) e despacho direto de kernel GPU (CUDA / ROCm / Metal) via runtime C++, com *fallback* automático em CPU.
5. **Padrões seguros.** Segredos vêm de `env`. Erros de indentação, tipo e dimensão de tensor são detectados em tempo de compilação com mensagens que ensinam.

Um programa Tilt mínimo:

```tilt
pipeline ola_dados:
  passos:
    - tabela = ler_csv "dados/vendas.csv"
    - resumo = tabela.agrupar_por "regiao", { total: somar "valor" }
    - escrever_parquet resumo, "saida/vendas_por_regiao.parquet"
```

---

## 2. Princípios de Design da Linguagem

| Princípio | O que significa na prática |
|-----------|---------------------------|
| **Legibilidade acima de concisão** | O código deve ser lido em voz alta sem esforço. Palavras-chave em português, sem pontuação supérflua. |
| **Progressive disclosure** | O caso simples é curto. Opções avançadas (dispositivo, sharding, quantização) são chaves extras, nunca obrigatórias. |
| **Baterias inclusas** | `stdlib` traz CSV/Parquet/JSON, SQL, DataFrame, HTTP, tensores, `nn`, cliente LLM, banco vetorial e orquestrador de agentes. |
| **Padrões seguros** | Assíncrono por padrão nas rotas. Sem alocação manual. Segredos só via `env`. Sem `panic` implícito. |
| **Tipos opcionais, inferência total** | `x = 3` já é `inteiro`. Anotações servem para contratos públicos (`tipo`, `entrada:`) e formas de tensor. |
| **Erros que ensinam** | Toda mensagem aponta a linha, mostra o esperado vs. o encontrado e sugere a correção. |
| **Uma forma óbvia** | Uma sintaxe de laço, uma de condicional, uma de função. Sem açúcar redundante. |

---

## 3. Sintaxe Essencial

### 3.1 Indentação, comentários e literais
- Indentação **rígida de 2 espaços**. Tabs são erro de compilação.
- Comentário de linha: `#`.
- Literais: `texto` (`"..."`), `inteiro` (`42`), `decimal` (`3.14`), `logico` (`verdadeiro` / `falso`), `nulo`.
- Texto multilinha e interpolação: `"""..."""` e `{{expressao}}`.

### 3.2 Variáveis e funções
```tilt
seja taxa = 0.001          # 'seja' é opcional; 'taxa = 0.001' também vale
constante MAX_TOKENS = 4096

funcao normalizar t: tensor -> tensor:
  media = t.media
  desvio = t.desvio_padrao
  retornar (t - media) / desvio
```

### 3.3 Controle de fluxo
```tilt
se pontuacao >= 0.9:
  rotulo = "alta"
senao se pontuacao >= 0.5:
  rotulo = "media"
senao:
  rotulo = "baixa"

para cada linha em tabela:
  imprimir linha.email

enquanto tentativas < 3:
  tentativas = tentativas + 1

tentar:
  resposta = perguntar gpt, usuario: pergunta
capturar erro:
  registrar "falha no LLM: {{erro.mensagem}}"
```

### 3.4 Módulos
```tilt
importar rede
importar dados como df
de agentes importar memoria_vetorial
```

### 3.5 Coleções
```tilt
nomes = ["ana", "bruno", "caio"]
config = { epocas: 10, lote: 64 }
primeiros = nomes[0..2]
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

```tilt
tipo EntradaInferencia:
  texto: texto
  vetor_contexto: tensor[f32, 1536]
  temperatura: decimal = 0.2      # valor padrão

tipo MetricasProcessamento:
  latencia_ms: decimal
  tokens_usados: inteiro
  dispositivo: texto
```

---

## 5. Engenharia de Dados

### 5.1 Fontes e conectores
```tilt
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
```tilt
pipeline etl_clientes:
  agenda: "0 * * * *"           # cron; ausente = sob demanda
  ao_falhar: repetir 3, espera: "30s"

  passos:
    - bruto = ler clientes
    - limpo = bruto
        .filtrar linha.email contem "@"
        .derivar { dominio: dividir linha.email, "@" [1] }
    - agregado = limpo.agrupar_por "dominio", {
        total: contar,
        receita: somar "valor"
      }
    - escrever agregado, para: parquet "s3://bucket/clientes_por_dominio.parquet"
```

### 5.3 Qualidade de dados
```tilt
verificar agregado:
  - nao_nulo: [dominio, total]
  - unico: dominio
  - intervalo: receita >= 0
  ao_violar: abortar        # ou 'avisar'
```

### 5.4 Streaming
```tilt
pipeline contagem_ao_vivo:
  entrada: eventos                 # fonte kafka
  janela: "1min"
  passos:
    - por_url = entrada.agrupar_por "url", { cliques: contar }
    - escrever por_url, para: postgres "clique_por_minuto"
```

---

## 6. Machine Learning Clássico

### 6.1 Experimentos
```tilt
experimento prever_churn:
  dados: tabela "dados/clientes.parquet"
  alvo: "churn"
  atributos: [uso_mensal, tickets_suporte, plano]
  pre_processar:
    - categoricas: [plano] -> um_de_n
    - numericas: [uso_mensal, tickets_suporte] -> padronizar
  dividir: { treino: 0.7, validacao: 0.15, teste: 0.15 }
  modelo: floresta_aleatoria
    arvores: 300
    profundidade_max: 12
  metricas: [acuracia, f1, auc, matriz_confusao]
  registrar_em: "mlflow://localhost:5000/churn"
```

Modelos nativos: `regressao_linear`, `regressao_logistica`, `floresta_aleatoria`, `gradiente_impulsionado`, `kmeans`, `knn`, `svm`.

### 6.2 Uso do modelo treinado
```tilt
servico Predicao:
  rota post "/prever":
    entrada: { cliente: mapa }
    passos:
      - p = experimento prever_churn.prever entrada.cliente
      - responder: { risco_churn: p.probabilidade }
```

---

## 7. Deep Learning e Tensores na GPU

### 7.1 Definição de modelo
```tilt
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
```

Camadas nativas: `densa`, `conv2d`, `agrupamento_max`, `abandono`, `norma_lote`, `norma_camada`, `atencao`, `incorporacao`, `recorrente`, `residual`, além de `ativacao: relu | gelu | silu | tanh | sigmoide`.

### 7.2 Treino
```tilt
treino Classificador:
  dados: carregador "dados/treino", lote: 64, embaralhar: verdadeiro
  validacao: carregador "dados/val", lote: 128
  perda: entropia_cruzada
  otimizador: adam
    taxa: 0.001
    decaimento: 0.01
  agendador: cosseno
  epocas: 20
  precisao: mista               # amp bf16/f16 automático
  parar_cedo:
    monitorar: val_perda
    paciencia: 3
  ao_epoca:
    - registrar { epoca: epoca, val_acuracia: metricas.val_acuracia }
```

O laço de treino, o `autograd`, o `.retropropagar` e o `.passo` do otimizador são **gerados automaticamente**. O bloco `ao_epoca` roda callbacks do usuário.

### 7.3 Tensores explícitos (controle fino)
```tilt
tarefa treinar_passo:
  entrada:
    lote_x: tensor[f32, 64, 1536] no dispositivo gpu
    alvos: tensor[i32, 64] no dispositivo gpu
  executar:
    - predicoes = modelo Classificador.para_frente lote_x
    - perda = perda_entropia_cruzada predicoes, alvos
    - perda.retropropagar
    - otimizador_adam.passo taxa: 0.001
```

O analisador semântico valida as formas (`tensor[f32, 64, 1536] @ densa[1536, 512]` → `tensor[f32, 64, 512]`) antes de gerar código.

### 7.4 Inferência e exportação
```tilt
modelo Classificador.exportar:
  formato: onnx                  # ou 'tilt' (nativo), 'gguf'
  quantizacao: i8
  saida: "modelos/classificador.onnx"
```

---

## 8. LLMs

### 8.1 Declaração do provedor
```tilt
llm gpt:
  provedor: "anthropic"          # anthropic | openai | local | vllm
  modelo: "claude-sonnet-5"
  temperatura: 0.2
  max_tokens: 1024
  chave: env "ANTHROPIC_API_KEY"
```

### 8.2 Chamada e prompts
```tilt
fluxo resumir:
  entrada: { documento: texto }
  passos:
    - resposta = perguntar gpt:
        sistema: "Você resume textos técnicos em 3 frases."
        usuario: "Resuma:\n\n{{documento}}"
    - retornar resposta.texto
```

### 8.3 Saída estruturada (garantida pelo tipo)
```tilt
tipo Resumo:
  titulo: texto
  pontos: lista[texto]
  sentimento: "positivo" | "neutro" | "negativo"

fluxo extrair:
  entrada: { texto: texto }
  passos:
    - dados = perguntar gpt, formato: Resumo:
        usuario: "Extraia estrutura de:\n{{texto}}"
    - retornar dados            # já validado contra 'Resumo'
```

### 8.4 Streaming
```tilt
servico Chat:
  rota post "/stream":
    entrada: { mensagem: texto }
    passos:
      - fluxo_tokens = perguntar_em_fluxo gpt, usuario: entrada.mensagem
      - responder_em_fluxo fluxo_tokens
```

### 8.5 Embeddings
```tilt
vetor = incorporar "text-embedding-3-small", "texto de exemplo"
```

---

## 9. RAG e Bancos Vetoriais

```tilt
indice base_conhecimento:
  embeddings: "text-embedding-3-small"
  armazenamento: "qdrant://localhost:6333/kb"   # qdrant | pgvector | memoria
  dimensao: 1536
  metrica: cosseno

pipeline indexar_docs:
  passos:
    - docs = ler_parquet "dados/artigos.parquet"
    - pedacos = docs.dividir_texto "conteudo", tamanho: 800, sobreposicao: 100
    - base_conhecimento.inserir pedacos

fluxo responder:
  entrada: { pergunta: texto }
  passos:
    - trechos = base_conhecimento.buscar entrada.pergunta, top_k: 5
    - resposta = perguntar gpt:
        sistema: "Responda usando SOMENTE o contexto. Se faltar, diga que não sabe."
        usuario: "Contexto:\n{{trechos}}\n\nPergunta: {{entrada.pergunta}}"
    - retornar { texto: resposta.texto, fontes: trechos.ids }
```

---

## 10. Agentes de IA

### 10.1 Ferramentas
```tilt
ferramenta busca_documentos:
  descricao: "Busca trechos relevantes na base de conhecimento."
  entrada:
    termo: texto
    limite: inteiro = 5
  executar:
    vetor = incorporar "text-embedding-3-small", termo
    retornar base_conhecimento.buscar vetor, top_k: limite

ferramenta clima:
  descricao: "Clima atual de uma cidade."
  entrada: { cidade: texto }
  executar:
    retornar http.obter "https://api.clima/v1", params: { q: cidade }
```

### 10.2 Agente
```tilt
agente AssistenteTecnico:
  llm: gpt
  papel: "Especialista em análise preditiva e dados estruturados."
  ferramentas: [busca_documentos, clima]
  memoria: conversa            # nenhuma | conversa | vetorial
  max_passos: 8
  ao_passo:
    - registrar { passo: passo.indice, ferramenta: passo.ferramenta }
```

### 10.3 Multi-agente
```tilt
equipe PesquisaEEscrita:
  agentes:
    - pesquisador: AssistenteTecnico
    - escritor: RedatorTecnico
  estrategia: supervisor       # sequencial | paralelo | supervisor
  supervisor: gpt
  objetivo: "Produzir relatório técnico com fontes citadas."
```

### 10.4 Exposição via serviço
```tilt
servico Agente:
  porta: 8080
  rota post "/chat":
    entrada: { mensagem: texto }
    passos:
      - r = AssistenteTecnico.responder entrada.mensagem
      - responder: { texto: r.texto, passos: r.rastro }
```

---

## 11. APIs e Serviços Assíncronos

```tilt
servico ApiPredicao:
  porta: 8080
  dispositivo: "cuda:0"
  meio:                         # middleware
    - registro_requisicoes
    - limite_taxa: { por_minuto: 120 }

  rota post "/v1/predizer":
    entrada: EntradaInferencia
    passos:
      - saida_modelo = modelo Classificador.executar entrada.vetor_contexto
      - responder:
          status: 200
          dados:
            classe: saida_modelo.classe_top1
            probabilidade: saida_modelo.probabilidade
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
│   │   ├── token.hpp
│   │   ├── lexer.hpp
│   │   └── lexer.cpp            # tokenizador + pilha INDENT/DEDENT
│   ├── parser/
│   │   ├── ast.hpp
│   │   ├── parser.hpp
│   │   └── parser.cpp
│   ├── semantic/
│   │   ├── type_checker.hpp
│   │   ├── type_checker.cpp     # inferência, escopos, formas de tensor
│   │   └── shape_solver.cpp     # álgebra de dimensões de tensor
│   ├── ir/
│   │   ├── ir_builder.hpp
│   │   ├── ir_instruction.hpp
│   │   └── bytecode.hpp         # formato serializável do IR
│   ├── interp/
│   │   ├── tree_interp.cpp      # Fase 1
│   │   └── vm.cpp               # Fase 2: VM de bytecode
│   ├── codegen/                 # Fase 3 (opcional)
│   │   ├── codegen_x86_64.cpp
│   │   └── codegen_arm64.cpp
│   └── runtime/
│       ├── arena.hpp            # alocador linear por requisição
│       ├── tensor.hpp          # shape, stride, ponteiro host/device
│       ├── tensor_ops.cpp      # matmul/conv/relu: AVX + threads
│       ├── gpu_runtime.hpp
│       ├── gpu_runtime.cpp      # dlopen de libcuda / ROCm / Metal
│       ├── autograd.cpp        # grafo reverso p/ 'treino'
│       ├── net_server.cpp      # epoll / kqueue
│       ├── data/
│       │   ├── table.cpp        # DataFrame colunar (Arrow)
│       │   ├── connectors.cpp   # postgres, kafka, s3, csv, parquet
│       │   └── pipeline.cpp     # executor de 'pipeline' + agenda
│       ├── llm/
│       │   ├── client.cpp       # provedores anthropic/openai/local
│       │   ├── structured.cpp   # saída via JSON Schema do 'tipo'
│       │   └── embeddings.cpp
│       └── agent/
│           ├── loop.cpp         # ciclo pensar-agir-observar
│           ├── memory.cpp       # conversa | vetorial
│           └── team.cpp         # sequencial | paralelo | supervisor
├── stdlib/
│   ├── io.tilt
│   ├── rede.tilt
│   ├── dados.tilt
│   ├── tensores.tilt
│   ├── nn.tilt
│   ├── llm.tilt
│   ├── vetor.tilt
│   └── agentes.tilt
└── exemplos/
    ├── api_predicao.tilt
    ├── treino_tensores.tilt
    ├── etl_clientes.tilt
    ├── rag_suporte.tilt
    └── agente_dados.tilt
```

---

## 14. Roteiro de Implementação Passo a Passo

### Fase 1 — Lexer baseado em linhas e indentação
- [ ] Leitura de buffer com `std::string_view` (zero alocação dinâmica no lexer).
- [ ] Máquina de estados de indentação: contar espaços no início da linha (múltiplos exatos de 2), comparar com `std::vector<size_t> indent_stack`, emitir `TOKEN_INDENT` / `TOKEN_DEDENT` / `TOKEN_NEWLINE`.
- [ ] Tabs → erro com mensagem clara. Reconhecer `chave:`, literais (texto, decimal, inteiro, `verdadeiro`/`falso`/`nulo`), `-`, `{ }` de mapa inline, `"""` multilinha, `{{ }}` de interpolação.

### Fase 2 — Parser dos blocos declarativos
- [ ] `ProgramNode` com declarações de alto nível.
- [ ] `tipo` → registro estruturado (com valores padrão e uniões literais `"a" | "b"`).
- [ ] `fonte`, `pipeline`, `modelo`, `treino`, `experimento`, `llm`, `indice`, `fluxo`, `ferramenta`, `agente`, `equipe`, `servico` → nós dedicados com metadados de execução.
- [ ] `passos:` → lista de instruções sequenciais; encadeamento `.metodo` e chamadas sem parênteses (`ler clientes`, `perguntar gpt, usuario: x`).
- [ ] Controle de fluxo: `se/senao`, `para cada`, `enquanto`, `tentar/capturar`, `retornar`, `funcao`.

### Fase 3 — Analisador semântico e validação de tensores
- [ ] Inferência de tipos ascendente; anotações opcionais viram contratos verificados.
- [ ] `shape_solver`: propagar formas por `densa`, `conv2d`, `atencao`, `@` (matmul); rejeitar incompatibilidades antes do codegen.
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
- [ ] Cache de bytecode em disco (`.tiltc`).

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
| DL | `camadas` `densa` `conv2d` `abandono` `ativacao` `perda` `otimizador` `epocas` `dispositivo` `tensor` `no dispositivo` `retropropagar` |
| LLM | `llm` `perguntar` `perguntar_em_fluxo` `sistema` `usuario` `formato` `incorporar` |
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
