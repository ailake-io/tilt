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
- **M3.5 / M4** — resolução de nomes em `passos:`, `shape_solver` de tensores,
  interpretador de árvore. Próximo.

`tilt checar` hoje valida: declarações duplicadas (T032), tipos desconhecidos
(T033), anotações de tensor malformadas (T034), segredos literais (T020),
`dispositivo:` inválido (T021) e `ferramentas:` de `agente` não declaradas
(T031). Resolução de variáveis dentro de `passos:`/`executar:` e o solver de
dimensões de tensor ainda não estão implementados.
