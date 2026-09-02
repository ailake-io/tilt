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
- **M2** — parser + AST. Próximo.

Parser, semântica e interpretador ainda não implementados.
