# 01 — Sintaxe

## Indentação

**2 espaços por nível, rígido.** Tab na indentação é erro (`T002`); indentação
que não é múltiplo de 2 é erro (`T001`). Blocos são abertos por `:` no fim da
linha e delimitados pela indentação — não há `{ }` nem `end`.

```tilt
pipeline exemplo:
  passos:
    - x = 1
```

## Comentários

`#` até o fim da linha. Linhas só com comentário ou em branco são ignoradas.

```tilt
# isto é um comentário
x = 1  # comentário no fim da linha
```

## Literais

| Tipo | Exemplos |
|---|---|
| texto | `"oi"`, `"linha\ncom escape"`, `"""multi\nlinha"""` |
| inteiro | `42`, `0`, `-7` (o `-` é operador unário) |
| decimal | `3.14`, `1.5e-3`, `2.0` |
| lógico | `verdadeiro`, `falso` |
| nulo | `nulo` |

Interpolação em texto: `"Olá {{nome}}"` substitui `nome` pelo valor no escopo
(nos textos executados; em anotações de tipo o texto fica literal).

## Variáveis

No nível de topo:

```tilt
seja taxa = 0.001
constante MAX_TOKENS = 4096
```

Dentro de `passos:` / `executar:` / corpo de `funcao`: basta atribuir.

```tilt
- a = 2
- b = a + 3
```

Escopo é léxico e aninhado (`se`, `para cada` etc. criam um sub-escopo).

## Funções

```tilt
funcao normalizar t: tensor -> tensor:
  media = t.media
  retornar (t - media) / t.desvio_padrao

funcao faixa n -> texto:
  se n >= 100:
    retornar "grande"
  retornar "pequena"
```

- Parâmetros: `nome` ou `nome: <tipo>`, separados por espaço ou vírgula.
- Tipo de retorno opcional após `->`.
- `retornar <expr>` (ou `retornar` sem valor → `nulo`).
- Chamada: `f(a, b)` (forma não ambígua) **ou** `f a, b` (estilo declarativo).

Funções cujo corpo cabe no subconjunto puro rodam numa VM de bytecode
automaticamente — ver [guia 09](guia-09-vm-nativo.md).

## Controle de fluxo

```tilt
se pontuacao >= 0.9:
  rotulo = "alta"
senao se pontuacao >= 0.5:
  rotulo = "media"
senao:
  rotulo = "baixa"

para cada linha em tabela:
  imprimir linha.email

enquanto tentativas < 3:
  tentativas = tentativas + 1

tentar:
  r = perguntar gpt, usuario: pergunta
capturar erro:
  registrar "falha:", erro
```

- `para cada <var> em <lista|tabela>` — `<var>` não pode ser `e`/`ou`/`nao`/`contem` (reservadas).
- `enquanto` tem guarda de 5 milhões de iterações (aborta com `T901`).
- `tentar/capturar` captura `T9xx` de execução; a variável do `capturar` recebe a mensagem.

## Operadores

| Precedência (baixa → alta) | Operadores |
|---|---|
| união de literais | `\|` (só em posição de tipo) |
| ou | `ou` |
| e | `e` |
| igualdade | `==` `!=` `contem` |
| comparação | `<` `<=` `>` `>=` |
| aditivo | `+` `-` |
| multiplicativo | `*` `/` `%` |
| unário | `nao` `-` |
| pós-fixo | `.campo` `?.campo` `[i]` `[a..b]` `f(...)` `f a, b` `no dispositivo <x>` |

- `+` com texto concatena. `contem`: `"abcd" contem "bc"` ou `lista contem valor`.
- `e`/`ou` fazem curto-circuito no interpretador (não na VM — ver guia 09).
- `x?.campo` retorna `nulo` se `x` não tiver o campo, em vez de erro.

## Coleções

```tilt
nomes = ["ana", "bruno", "caio"]
config = { epocas: 10, lote: 64 }
primeiro = nomes[0]
fatia = nomes[0..2]        # ["ana", "bruno"]
```

> Listas e mapas literais precisam caber em **uma linha** (limitação da 1ª passada).

## Importar

```tilt
importar rede
de agentes importar memoria_vetorial
```

> A resolução de módulos ainda não carrega uma stdlib; `importar` registra o
> nome mas não executa nada.
