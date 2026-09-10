# 09 — VM de bytecode e codegen nativo

## VM de bytecode (automática)

Toda `funcao` cujo corpo cabe no **subconjunto puro** é compilada para bytecode
de pilha (`src/vm/`) e executada pela VM em vez do interpretador de árvore, com
cache por função. É transparente — não há flag.

Subconjunto suportado:

- literais `inteiro`, `decimal`, `texto` (sem `{{ }}`), `logico`, `nulo` e
  **listas** (`[...]`);
- variáveis locais e parâmetros;
- `+ - * / %`, comparações, `contem`, `e` / `ou` / `nao`;
- **índice** `lista[i]`;
- `se` / `senao se` / `senao`, `enquanto`, **`para cada`**, `retornar`;
- chamadas a outras `funcao`s (recursão inclusive) e a `imprimir` / `tamanho`.

Fora disso (`tentar`, membros, tensores, LLM, agente, tabelas, interpolação,
…) a função cai no interpretador de árvore.

`e` / `ou` compilam com curto-circuito (Fase 8): o lado direito só é
avaliado quando o esquerdo não decide, e o resultado é sempre `logico` —
mesma semântica do interpretador de árvore.

`TILT_VM_DEBUG=1 tilt executar prog.tilt` despeja o bytecode gerado.

## `tilt executar --vm`

Roda **pipelines pelo bytecode VM**: cada pipeline cujo `passos:` cabe no
subconjunto acima (sem `agenda:` / `ao_falhar:`) é compilado para bytecode e
executado pela VM; os demais caem no interpretador de árvore, pipeline a
pipeline. A saída é idêntica à de `tilt executar` — todo o suíte de golden
`run-*` passa nos dois modos.

## Codegen nativo — `tilt compilar`

```bash
tilt compilar prog.tilt --saida ./prog        # ELF da arquitetura do host
./prog
tilt compilar prog.tilt --saida ./prog --arch arm64   # backend explicito
```

Compila o **programa inteiro** quando ele cabe no subconjunto da VM:

- `funcao`s (com `funcao principal`) **ou** programas de `pipeline`s — o
  ponto de entrada espelha o interpretador: pipelines rodam em ordem com o
  cabeçalho `== pipeline N ==`, senão `funcao principal`;
- valores completos: `inteiro`, `decimal`, `texto`, `logico`, `nulo`,
  **listas** com índice e `tamanho`;
- `se` / `senao` / `enquanto` / `para cada`, recursão e chamadas entre
  `funcao`s, `imprimir` com qualquer valor do subconjunto.

### Backends

Há dois backends emitindo Assembly GNU as (ELF), com o mesmo subconjunto e o
mesmo runtime C (`--asm` mantém os intermediários `.s` + `.rt.c`, link via
`$CC -O2 … -lm`):

| backend   | alvo                     | convenções                                                    |
|-----------|--------------------------|---------------------------------------------------------------|
| `x86_64`  | x86-64 (AT&T, `-no-pie`) | args `%rdi/%rsi`, pilha de slots de 48 bytes em `%rsp`        |
| `arm64`   | AArch64 (AAPCS)          | args `x0-x7`, retorno `x0`, callee-saved `x19-x28`, `bl`, sp 16-alinhado |

`--arch x86_64|arm64` escolhe o backend; o padrão `auto` detecta a
arquitetura do host em compile-time. Alvo cruzado (ex.: `--arch arm64` num
host x86-64) exige toolchain no PATH: `aarch64-linux-gnu-gcc` /
`x86_64-linux-gnu-gcc` ou o override por env `CC_AARCH64` / `CC_X86_64` —
sem isso, erro claro antes de chamar o compilador.

Rejeita, com mensagem clara: passo fora do subconjunto (chamadas builtin como
`ler_csv`, interpolação, membros), constante/operador fora do subconjunto,
pipeline com `agenda:` / `ao_falhar:`, programas sem pipeline nem
`funcao principal` e arquitetura desconhecida em `--arch`.

### Semântica idêntica ao interpretador

O runtime C replica `apply_binop` / `to_display` / `truthy` de
`runtime/value.cpp` (concatenação de texto via `+`, `contem`, comparação
lexicográfica de textos, divisão por zero → 0, `/` sempre `decimal`, formatação
`%g` de decimais). O teste `native` do `ctest` é **diferencial**: compara a
saída do binário nativa com a do interpretador para cada fixture
(`tests/fixtures/nativo.tilt`, `nativo2.tilt`). O teste `native_arm64` gera o
ASM AArch64 de um fixture, confere o prologo AAPCS/chamadas ao runtime e o
monta com um assembler cross (`clang --target=aarch64-linux-gnu` ou
`aarch64-linux-gnu-as`); com `qemu-aarch64` + linker cross instalados também
linka e roda o binário sob qemu-user comparando com o interpretador — sem a
toolchain completa, essa etapa é pulada (validação end-to-end fica para CI /
máquina ARM).

### Exemplo

```tilt
funcao fib n -> inteiro:
  se n < 2:
    retornar n
  retornar fib(n - 1) + fib(n - 2)

pipeline calcular:
  passos:
    - imprimir fib(20)                 # 6765
    - precos = [9.5, 4.25]
    - total = 0.0
    - para cada p em precos:
        total = total + p
    - imprimir "total:", total         # total: 13.75
```

Ver [`../exemplos/nativo.tilt`](../exemplos/nativo.tilt) e
[`../tests/fixtures/nativo2.tilt`](../tests/fixtures/nativo2.tilt).
