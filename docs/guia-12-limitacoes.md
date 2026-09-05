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

- Parquet é nativo (reader/writer próprio, zero dependências) mas de 1ª
  passada: colunas obrigatórias (sem nulls), encoding PLAIN, sem compressão,
  um row group por arquivo. Arquivos fora desse perfil (compressão, OPTIONAL,
  dictionary) levantam erro claro na leitura.
- Delta Lake é mínimo: `escrever_delta` sobrescreve a tabela (recria a versão
  0); o append existe via `anexar_delta` (nova versão por commit atômico de
  `rename`, validação de schema, single-writer — sem locks/optimistic
  concurrency), sem partições nem checkpoints; a leitura herda as limitações
  do Parquet acima, então tabelas de outros escritores só leem sem
  compressão/dictionary e com colunas obrigatórias.
- Conectores ainda não cobertos (`kafka`, `s3`, `mongodb`, `iceberg`) →
  `T900`. CSV, JSON, Parquet, Delta, SQLite, Postgres, Redis, Qdrant e
  pgvector rodam.
- Bancos relacionais: somente consultas SELECT (sem INSERT/UPDATE via SQL,
  sem prepared statements); Postgres carrega `libpq.so.5` e SQLite
  `libsqlite3.so.0` via `dlopen` — precisam estar instalados no sistema.
- Redis: sem TLS/AUTH/db index, um comando por conexão, timeout fixo de 5s.
- Qdrant: a coleção usa distância Cosine e ids determinísticos derivados do
  id tilt; `buscar` contra Qdrant devolve `id` e `score` (sem o campo
  `texto`, que fica no payload do ponto).
- pgvector: exige a extensão `vector` instalada no banco (o Tilt tenta
  `CREATE EXTENSION IF NOT EXISTS vector`, que precisa de privilégio na
  primeira vez); upsert sem prepared statements (escaping manual de
  strings); nome de coleção restrito a `[a-z0-9_]`.
- Streaming com `janela:` não roda.
- `--agendar` entra em loop real de agenda, mas o parser cron é numérico
  (sem nomes `jan`/`mon`), os campos dia-do-mês e dia-da-semana combinam por
  E (não pelo OU do cron clássico) e não há persistência de estado entre
  disparos.

## ML / DL

- `pesos: "arquivo"` carrega no formato tilt-pesos (ver guia 04); arquivo
  ausente mantém o init Xavier com `[nota]`.
- `treino` suporta `perda: entropia_cruzada` (com `softmax` final) e
  `perda: quadratica` (regressão escalar); `gelu` no backward é aproximada
  como identidade.
- `norma_camada` funciona na inferência (sem affine); `treino` com ela →
  erro (backward ainda não existe). `conv2d`/`norma_lote` → erro claro.
- GPU: o backend CUDA (`TILT_GPU=auto`) só foi validado em hardware; aqui use
  `TILT_GPU=fake` para exercitar o caminho de dispatch.

## LLM / RAG

- Sem `TILT_LLM`, a chamada real depende do `curl` no `PATH`.
- `indice` roda com `armazenamento: "memoria"` (cosseno local),
  `"qdrant://host:porta/colecao"` (REST via curl) e `"pgvector://colecao"`
  (SQL sobre libpq, cosseno `<=>`; a tabela é criada automaticamente e
  `buscar` devolve `{ id, score }`, sem o texto).
- Os embeddings do modo `mock` são um bag-of-tokens hasheado (16 dimensões) —
  bons para testes determinísticos, não para relevância real.

## Agentes

- O planner usa um protocolo simples (uma linha por turno: `chamar ...` /
  `responder: ...`); LLMs reais podem ignorá-lo — a resposta fora do
  protocolo vira a resposta final, sem garantia de que as ferramentas certas
  foram chamadas. No modo `mock` o planner é determinístico (cada ferramenta
  uma vez, na ordem declarada).
- Supervisor delega por rótulo; um rótulo sugerido pelo LLM que não está em
  `agentes:` é erro de execução (`T901`).

## HTTP

- As rotas executam em paralelo por padrão (pool de `min(4, núcleos)`
  workers; `--threads N` ajusta, `--threads 1` volta ao serial). Rotas que
  tocam o **mesmo** `indice` em memória se serializam por um mutex global do
  índice — para alta concorrência, use Qdrant/pgvector como armazenamento.
- No Linux: epoll + keep-alive + arena por requisição + pool de rotas com
  ordenação por sequência por conexão (M10.2 + paralelismo entregues).
  Em outros sistemas, o servidor é bloqueante, uma conexão por vez,
  `Connection: close`.

## VM / nativo

- A VM cobre `funcao` pura e `pipeline`s no subconjunto (literais incl.
  listas, `para cada`, índice, `contem`); o resto roda no interpretador de
  árvore (`tilt executar --vm` cai por pipeline, transparente).
- `e` / `ou` na VM fazem curto-circuito (desde a Fase 8), com resultado
  sempre `logico`.
- `tilt compilar` cobre o **programa inteiro** dentro do subconjunto da VM:
  `funcao principal` ou pipelines, com texto/decimal/lista e saída idêntica
  ao interpretador (runtime C espelhando `value.cpp`; teste `native`
  diferencial). Fora do subconjunto (builtins como `ler_csv`, interpolação,
  membros, `agenda:`/`ao_falhar:`) rejeita com mensagem clara.

## Plataforma

- `tilt compilar` gera x86-64; em ARM o teste `native` é pulado.
- Binário estático de libstdc++ só no Linux (no macOS usa a libc++ do sistema).
