# Tilt

Compilador e runtime da linguagem **Tilt** — sintaxe declarativa estilo YAML, focada em
Machine Learning, Deep Learning, Engenharia de Dados, LLMs e Agentes de IA.
Escrito em C++20, sem LLVM e sem Rust.

- Especificação da linguagem: [`CLAUDE.md`](CLAUDE.md)
- Plano de implementação: seção 14 do `CLAUDE.md`

## Build

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
- **M7+** — GPU, carga de `pesos:`, conectores de rede, LLM, agentes, VM de bytecode.

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
(relu/sigmoide/tanh/silu). Pós-treino os pesos ficam no `modelo`. Carga de
`pesos:` de arquivo e GPU: M7. Conectores de rede, LLM, agentes: adiante.

Conectores (`postgres`, `kafka`, `s3`, `parquet`), LLM (`perguntar`,
`incorporar`), agentes, `modelo`/`treino` e GPU levantam um erro de execução
apontando o marco que os implementará (`TILT_LLM=mock` simula o LLM).
