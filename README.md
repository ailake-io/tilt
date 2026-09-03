# Tilt

Compilador e runtime da linguagem **Tilt** — sintaxe declarativa estilo YAML, focada em
Machine Learning, Deep Learning, Engenharia de Dados, LLMs e Agentes de IA.
Escrito em C++20, sem LLVM e sem Rust.

- Especificação da linguagem: [`CLAUDE.md`](CLAUDE.md)
- Plano de implementação: seção 14 do `CLAUDE.md`

## Instalação

### Do código-fonte

```bash
git clone https://github.com/ailake-io/tilt
cd tilt
sh scripts/install.sh                 # instala em ~/.local
# ou:  sh scripts/install.sh --prefix=/usr/local   (talvez com sudo)
```

Precisa de `cmake >= 3.20` e um compilador C++20 (GCC 11+/Clang 14+).
Coloca `tilt` em `<prefix>/bin` e os exemplos em `<prefix>/share/tilt`.
Remoção: `sh scripts/install.sh --uninstall`.

### Binários pré-compilados

Cada tag `v*` publica `tilt-<versao>-<os>-<arch>.tar.gz` em
[Releases](https://github.com/ailake-io/tilt/releases) (Linux x86_64 estático,
macOS arm64). Extraia e ponha `bin/tilt` no `PATH`. `curl` é necessário em
tempo de execução apenas para chamadas reais de LLM.

### Pacote local

```bash
cmake --preset release && cmake --build --preset release
cd build/release && cpack          # gera tilt-<versao>-<os>-<arch>.tar.gz
```

## Build (desenvolvimento)

```bash
cmake --preset release
cmake --build --preset release
./build/release/bin/tilt versao
```

Preset `debug` habilita AddressSanitizer, UBSan e `-Werror`.

## Testes

```bash
ctest --preset release
```

Testes de ouro em `tests/golden/`. Para regenerar os esperados:

```bash
UPDATE=1 sh tests/run_golden.sh ./build/release/bin/tilt tests/golden
```

## Estado

- **M0** — andaimes de build, CLI, módulo de diagnósticos, harness de teste. ✅
- **M1** — lexer + motor de indentação (`tilt tokens <arquivo>`). ✅
- **M2** — parser de descida recursiva + AST (`tilt ast <arquivo>`). ✅
- **M3** — análise semântica, 1ª passada (`tilt checar <arquivo>`). ✅
- **M4** — interpretador de árvore (`tilt executar <arquivo>`). ✅
- **M5** — executor de `pipeline`: `verificar`, `ao_falhar`, `agenda`. ✅
- **M5.2** — conector JSON + wiring de `fonte` para arquivos locais (csv/json). ✅
- **M6** — tensores + inferência de `modelo` (CPU). ✅
- **M6.2** — `treino`: backprop + SGD/Adam + `carregador`. ✅
- **M8** — cliente LLM (`perguntar`/`incorporar`), saída estruturada, RAG (`indice`). ✅
- **M9** — `ferramenta` executável, `agente.responder`, `equipe`. ✅ (1ª passada)
- **M10** — `servico` / `rota` HTTP (`tilt servir`). ✅ (1ª passada)
- **M7 / M9.2 / M10.2 / M11+** — GPU, planner iterativo, `supervisor`, epoll +
  keep-alive + `meio:`, VM de bytecode, codegen nativo.

### `tilt executar`

Roda todo `pipeline` de topo (e uma `funcao principal` se não houver pipeline).
Suportado hoje: cômputo puro, `se`/`para cada`/`enquanto`/`tentar`, `funcao`,
`ler_csv`/`escrever_csv`, `.filtrar`/`.agrupar_por`/`.derivar`/`.selecionar`
sobre tabelas, `imprimir`/`registrar`/`env`/`dividir`/`intervalo`/`somar`/…,
interpolação `{{var}}` em textos.

Executor de `pipeline`: passo `- verificar <tabela>:` com regras `nao_nulo` /
`unico` / `intervalo` e `ao_violar: abortar|avisar` (violação → `T910`);
`ao_falhar: repetir N` reexecuta os passos; `agenda: "<cron 5 campos>"` é
validada e, com `--agendar`, reconhecida. Métodos de tabela: `.ordenar_por` (com
`desc:`), `.limite`, `.distinto`.

Conector local: `ler_json`/`escrever_json` (parser/writer próprios) e
`fonte X: tipo: csv|json, caminho: "..."` lido por `ler X` num pipeline
(`caminho:` aceita `env "VAR"`). `tipo: postgres|kafka|s3` → `T900` (M5.3).

Tensores (f32, CPU): `tensor [..]` / `zeros [..]` / `uns [..]` / `aleatorio [..]`;
`+ - * /` elementwise e com escalar; `.forma .matmul .transposta .reformar
.relu/.gelu/.sigmoide/.tanh .softmax .soma .media .argmax`. `modelo M:` com
`camadas:` (`densa: N` / `linear: [e, s]` / `ativacao:` / `softmax` / `abandono:`)
roda via `modelo M.executar <tensor>` — init Xavier com semente fixa.

`treino M:` treina o `modelo M` de mesmo nome: `dados: carregador "d.csv",
alvo: "col"`, `perda: entropia_cruzada`, `otimizador: sgd|adam`, `taxa:`,
`epocas:`, `verboso:`. Backprop escrito à mão para a pilha densa+ativação+softmax
(relu/sigmoide/tanh/silu). Pós-treino os pesos ficam no `modelo`.

### LLM e RAG

`llm gpt: provedor: ..., modelo: ..., chave: env "..."` + `perguntar gpt:
sistema: ..., usuario: ...` → `{ texto, modelo }`. Saída estruturada:
`perguntar gpt, formato: MeuTipo:` → `Mapa` com os campos de `tipo MeuTipo`.
`incorporar "modelo", texto` → `tensor[f32, D]`. `dividir_texto texto,
tamanho:, sobreposicao:` → `lista` de trechos.

`indice kb: embeddings: "...", armazenamento: "memoria"` — `kb.inserir <lista|
texto>` e `kb.buscar "consulta", top_k: N` → `lista` de `{ id, texto, score }`
(cosseno). `armazenamento: qdrant://…` / `pgvector` → `T900` (M8.2).

Transporte: `TILT_LLM=mock` dá respostas/embeddings determinísticos (offline,
usado nos testes); sem a variável, chamadas reais via `curl` (Anthropic/OpenAI/
local). GPU e carga de `pesos:`: M7.

### Agentes

`ferramenta T: entrada: {...}, executar: <passos com retornar>` — chamável
direto (`T arg: v`) ou por um agente. `agente A: llm:, papel:, ferramentas:,
memoria: nenhuma|conversa, max_passos:` — `A.responder "msg"` roda as
ferramentas (laço determinístico no `mock`), junta as observações e pede a
resposta ao LLM → `{ texto, rastro: [{ passo, ferramenta, observacao }] }`.
`equipe E: agentes: [-rotulo: A], estrategia: sequencial|paralelo` —
`E.executar "msg"`. Planner iterativo e `supervisor`: M9.2.

### Serviço HTTP

`tilt servir <arquivo> [--porta N] [--requisicoes N]` sobe o `servico` declarado
(servidor TCP bloqueante; epoll + keep-alive + `meio:` no M10.2). Cada `rota
<metodo> "/caminho":` casa método+caminho; o corpo JSON vira `entrada` no escopo;
se a rota declara `entrada: <Tipo>`, campos ausentes → `400`; `- responder:
status:, dados:` monta a resposta JSON; rota não encontrada → `404`; exceção no
handler → `500`.

Conectores (`postgres`, `kafka`, `s3`, `parquet`), LLM (`perguntar`,
`incorporar`), agentes, `modelo`/`treino` e GPU levantam um erro de execução
apontando o marco que os implementará (`TILT_LLM=mock` simula o LLM).
