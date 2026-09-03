# 09 — VM de bytecode e codegen nativo

## VM de bytecode (automática)

Toda `funcao` cujo corpo cabe no **subconjunto puro** é compilada para bytecode
de pilha (`src/vm/`) e executada pela VM em vez do interpretador de árvore, com
cache por função. É transparente — não há flag.

Subconjunto suportado:

- literais `inteiro`, `decimal`, `texto` (sem `{{ }}`), `logico`, `nulo`;
- variáveis locais e parâmetros;
- `+ - * / %`, comparações, `e` / `ou` / `nao`;
- `se` / `senao se` / `senao`, `enquanto`, `retornar`;
- chamadas a outras `funcao`s (recursão inclusive) e a `imprimir` / `tamanho`.

Fora disso (`para cada`, `tentar`, membros, índices, tensores, LLM, agente,
tabelas, interpolação, …) a função cai no interpretador de árvore.

Diferença observável: **`e` / `ou` na VM não fazem curto-circuito** nesta
passada (avaliam os dois lados). Como o subconjunto não tem efeitos colaterais
em expressão além de `imprimir`, o *valor* é o mesmo.

`TILT_VM_DEBUG=1 tilt executar prog.tilt` despeja o bytecode gerado.

## Codegen nativo — `tilt compilar`

```bash
tilt compilar prog.tilt --saida ./prog        # ELF x86-64
./prog
```

Compila o **subconjunto inteiro puro**:

- `funcao`s com literais **inteiros**, `+ - * / %` e comparações/lógica;
- `se` / `enquanto` / `retornar`, recursão, chamadas entre `funcao`s (≤ 6 args);
- `imprimir` de inteiros;
- **exige** uma `funcao principal` (ponto de entrada).

Emite Assembly AT&T (máquina de pilha em slots de 16 bytes, `%rsp` 16-alinhado
para `call`), grava um `.s` e um `.rt.c` (runtime de uma função,
`tilt_print_row`) e chama `$CC -O2 -no-pie`. `--asm` mantém os intermediários.

Rejeita, com mensagem clara: constante não inteira, operador não suportado,
`tamanho`, chamada fora do subconjunto, ausência de `funcao principal`.

### Diferença conhecida

No nativo `/` é **divisão inteira** (`100 / 7 == 14`); no interpretador `/`
promove para `decimal` (`100 / 7 == 14.2857`). Programas que passam pelo
`native` de teste evitam `/` ou usam valores exatos.

### Exemplo

```tilt
funcao fib n -> inteiro:
  se n < 2:
    retornar n
  retornar fib(n - 1) + fib(n - 2)

funcao principal:
  imprimir fib(20)          # 6765
```

Ver [`../exemplos/nativo.tilt`](../exemplos/nativo.tilt).
