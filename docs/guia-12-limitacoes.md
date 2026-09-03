# 12 — Limitações (1ª passada)

Cada marco `M0`–`M12` foi entregue em "1ª passada": o caminho principal
funciona, mas há bordas conhecidas. Lista do que **ainda não** funciona.

## Sintaxe / parser

- Listas `[...]` e mapas `{...}` literais precisam caber em **uma linha**.
  Linhas de continuação indentadas dentro de `[` quebram o parser.
- `e` / `ou` / `nao` / `contem` são reservadas — não servem como nome de
  variável, parâmetro ou loop var.
- Assinatura de `funcao` com parâmetros compostos é reconhecida de forma
  simples; casos exóticos podem se perder.

## Semântica

- `tilt checar` **não** resolve nomes dentro de `passos:` / `executar:` (evita
  falso-positivo com variáveis implícitas como `linha`, `epoca`). `T030` só
  aparece em contextos limitados.
- O solver de dimensões de matmul/`densa` (`T012` completo) ainda não existe —
  só a validação da anotação `tensor[...]` (`T034`).

## Dados

- Conectores de rede (`postgres`, `kafka`, `s3`, `delta`) e Parquet → `T900`.
  Só CSV e JSON de arquivo local.
- Streaming com `janela:` não roda.
- `agenda:` é validada mas `--agendar` não entra em loop (roda uma vez).

## ML / DL

- `pesos: "arquivo"` não carrega — o `modelo` sempre inicia com Xavier
  (semente fixa). Não há `.salvar_pesos`.
- `treino` só suporta `perda: entropia_cruzada` com `softmax` na última camada;
  `gelu` no backward é aproximada como identidade.
- Camadas `conv2d`, `norma_lote`, `norma_camada` são ignoradas na inferência.
- GPU: o backend CUDA (`TILT_GPU=auto`) só foi validado em hardware; aqui use
  `TILT_GPU=fake` para exercitar o caminho de dispatch.

## LLM / RAG

- Sem `TILT_LLM`, a chamada real depende do `curl` no `PATH`.
- `indice` só com `armazenamento: "memoria"`; `qdrant://` / `pgvector` → `T900`.
- Os embeddings do modo `mock` são um bag-of-tokens hasheado (16 dimensões) —
  bons para testes determinísticos, não para relevância real.

## Agentes

- O laço do `agente` é determinístico (roda cada ferramenta uma vez); não há
  planner iterativo guiado pelo LLM.
- `equipe` com `estrategia: supervisor` → erro.

## HTTP

- Servidor bloqueante, uma requisição por vez. Sem epoll, keep-alive nem
  arena por requisição. `meio:` (middleware) só é reconhecido.

## VM / nativo

- A VM cobre só `funcao` pura; o resto roda no interpretador de árvore.
- `e` / `ou` na VM não fazem curto-circuito.
- `tilt compilar` cobre só o subconjunto **inteiro**; `/` é divisão inteira.
  Sem `funcao principal` não compila.

## Plataforma

- `tilt compilar` gera x86-64; em ARM o teste `native` é pulado.
- Binário estático de libstdc++ só no Linux (no macOS usa a libc++ do sistema).
