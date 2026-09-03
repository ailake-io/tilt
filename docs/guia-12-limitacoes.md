# 12 — Limitações (1ª passada)

Cada marco `M0`–`M12` foi entregue em "1ª passada": o caminho principal
funciona, mas há bordas conhecidas. Lista do que **ainda não** funciona.

## Sintaxe / parser

- `e` / `ou` / `nao` / `contem` são reservadas — não servem como nome de
  variável, parâmetro ou loop var.
- Assinatura de `funcao` com parâmetros compostos é reconhecida de forma
  simples; casos exóticos podem se perder.

## Semântica

- `tilt checar` resolve nomes dentro de `passos:` / `executar:` (`T030`):
  escopo global mais variáveis implícitas (`linha`, `entrada`, `epoca`,
  `metricas`, `passo`, `resultado`) e campos de `entrada:`. Nomes fora
  disso são reportados.
- O solver de formas (`T012`) cobre a cadeia `densa`/`linear` — propaga a
  dimensão corrente a partir da anotação `entrada: tensor[...]` e rejeita
  `linear: [a, b]` com `a` incompatível. `conv2d`, `norma_lote` e
  `norma_camada` ficam fora do solver.
- Não há inferência completa de tipos: anotações são validadas como
  contratos, mas os tipos não são propagados entre expressões.

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

- As rotas executam em série (na thread do event loop) — o interpretador não
  é reentrante; execução paralela de rotas fica para uma próxima passada.
- `meio:` (middleware) só é reconhecido.
- No Linux: epoll + keep-alive + arena por requisição (M10.2 entregue).
  Em outros sistemas, o servidor é bloqueante, uma conexão por vez,
  `Connection: close`.

## VM / nativo

- A VM cobre só `funcao` pura; o resto roda no interpretador de árvore.
- `e` / `ou` na VM não fazem curto-circuito.
- `tilt compilar` cobre só o subconjunto **inteiro**; `/` é divisão inteira.
  Sem `funcao principal` não compila.

## Plataforma

- `tilt compilar` gera x86-64; em ARM o teste `native` é pulado.
- Binário estático de libstdc++ só no Linux (no macOS usa a libc++ do sistema).
