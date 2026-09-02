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
- **M5+** — conectores de dados reais, tensores/DL, LLM, agentes, VM de bytecode.

### `tilt executar`

Roda todo `pipeline` de topo (e uma `funcao principal` se não houver pipeline).
Suportado hoje: cômputo puro, `se`/`para cada`/`enquanto`/`tentar`, `funcao`,
`ler_csv`/`escrever_csv`, `.filtrar`/`.agrupar_por`/`.derivar`/`.selecionar`
sobre tabelas, `imprimir`/`registrar`/`env`/`dividir`/`intervalo`/`somar`/…,
interpolação `{{var}}` em textos.

Conectores (`postgres`, `kafka`, `s3`, `parquet`), LLM (`perguntar`,
`incorporar`), agentes, `modelo`/`treino` e GPU levantam um erro de execução
apontando o marco que os implementará (`TILT_LLM=mock` simula o LLM).
