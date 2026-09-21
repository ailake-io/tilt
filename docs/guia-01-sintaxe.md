# 01 — Sintaxe

## Indentação

**Um estilo por arquivo: 2 espaços OU 1 tab por nível.** O estilo é definido
pela primeira linha indentada do arquivo e vale até o fim: uma linha depois
indentada com o outro estilo (ou um prefixo misturando tab e espaços) é erro
(`T002`). Com espaços, o nível precisa ser múltiplo de 2 (`T001`); com tabs,
1 tab = 1 nível (a largura do tab no editor não importa). Arquivo sem nenhuma
linha indentada aceita qualquer estilo. Blocos são abertos por `:` no fim da
linha e delimitados pela indentação — não há `{ }` nem `end`.

```tilt run
# Indentação de 2 espaços por nível.
pipeline exemplo:
  passos:
    - x = 1
    - imprimir x   # 1
```

```tilt run
# O mesmo programa com 1 tab por nível (não misture os dois no mesmo arquivo).
pipeline exemplo:
	passos:
		- x = 1
		- imprimir x   # 1
```

## Comentários

`#` até o fim da linha. Linhas só com comentário ou em branco são ignoradas.

```tilt run
pipeline comentarios:
  passos:
    # isto é um comentário (linha inteira)
    - x = 1  # comentário no fim da linha
    - imprimir x   # 1
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

```tilt run
pipeline literais:
  passos:
    - nome = "mundo"
    - imprimir "Olá {{nome}}"   # interpolação: Olá mundo
    - imprimir 42               # inteiro
    - imprimir 3.14             # decimal
    - imprimir verdadeiro       # logico
    - imprimir nulo             # nulo
```

## Variáveis

No nível de topo, `seja` é opcional e `constante` não pode ser reatribuída:

```tilt run
seja taxa = 0.001
constante MAX_TOKENS = 4096

pipeline vars:
  passos:
    - imprimir taxa * 2    # 0.002
    - imprimir MAX_TOKENS  # 4096
```

Dentro de `passos:` / `executar:` / corpo de `funcao`: basta atribuir.

```tilt run
pipeline atrib:
  passos:
    - a = 2        # cria a variável no escopo dos passos
    - b = a + 3
    - imprimir b   # 5
```

Escopo é léxico e aninhado: `se`, `para cada` etc. criam um sub-escopo, então
atribuir **só dentro** do ramo não vaza para fora — declare antes e atribua
dentro (ver exemplo do `se` abaixo).

## Funções

```tilt run
# Parâmetros com ou sem tipo; retorno opcional após '->'.
funcao faixa n -> texto:
  se n >= 100:
    retornar "grande"
  retornar "pequena"

funcao soma_texto a: texto, b: texto -> texto:
  retornar a + b

pipeline funcoes:
  passos:
    - imprimir faixa 42            # pequena (chamada sem parênteses)
    - imprimir faixa 200           # grande
    - imprimir soma_texto("x", "y")  # xy (com parênteses, sem ambiguidade)
```

- Parâmetros: `nome` ou `nome: <tipo>`, separados por espaço ou vírgula.
  Parâmetro opcional/composto: `nome[]` (sem tipo) ou `nome[]: <tipo>`
  (ex.: `limite[]: texto`).
- Valor padrão: `nome = <expr>` ou `nome: <tipo> = <expr>` (ex.: `funcao
  saudar nome, saudacao = "Ola":`). O padrão é avaliado a cada chamada e enxerga
  os parâmetros anteriores (`funcao area base, altura = base:`). Prefira
  literais; um nome como padrão precisa de vírgula antes do próximo parâmetro.
  Funções com padrão rodam pelo interpretador de árvore (não pela VM).
- Tipo de retorno opcional após `->` (inclui `mapa` como tipo base).
- `retornar <expr>` (ou `retornar` sem valor → `nulo`).
- Chamada: `f(a, b)` (forma não ambígua) **ou** `f a, b` (estilo declarativo).

### Funções anônimas

`funcao x, y: <expressão>` cria um valor-função. O corpo é uma única expressão e
as variáveis visíveis na criação são capturadas **por valor**. Chame com `f(x)`
(ou `f(a)(b)` quando uma função devolve outra) e passe às funções de ordem
superior `mapear`, `filtrar`, `reduzir`, `qualquer` e `todos`:

```tilt run
funcao somador n:
  retornar funcao x: x + n

pipeline anonimas:
  passos:
    - dobro = funcao x: x * 2
    - imprimir dobro(4)                                        # 8
    - imprimir mapear([1, 2, 3], funcao x: x * x)              # [1, 4, 9]
    - imprimir filtrar([1, 2, 3, 4], funcao x: x % 2 == 0)     # [2, 4]
    - imprimir reduzir([1, 2, 3], funcao acc, x: acc + x, 0)   # 6
    - imprimir somador(5)(1)                                   # 6
```

`mapear`/`filtrar` como *função* (`mapear(lista, f)`) não se confundem com os
métodos de tabela `t.mapear { col: expr }` / `t.filtrar cond`. O `tilt checar`
resolve os nomes do corpo (parâmetros mais o escopo visível). Limites: só corpo
em expressão (sem blocos), aridade exata e sem passar uma `funcao` nomeada
diretamente como valor (embrulhe: `funcao x: minha(x)`).

Funções cujo corpo cabe no subconjunto puro rodam numa VM de bytecode
automaticamente — ver [guia 09](guia-09-vm-nativo.md).

## Controle de fluxo

```tilt run
pipeline fluxo:
  passos:
    # se/senao: o 'senao' alinha com o '- se' (4 espaços), o corpo indenta +2.
    - pontuacao = 0.7
    - rotulo = "?"          # declara antes: ramo cria sub-escopo, não vaza
    - se pontuacao >= 0.9:
        rotulo = "alta"
    senao se pontuacao >= 0.5:
        rotulo = "media"
    senao:
        rotulo = "baixa"
    - imprimir rotulo       # media
    # para cada: percorre lista ou tabela; 'linha' é a variável do item.
    - tabela = [{ email: "a@x" }, { email: "b@x" }]
    - para cada linha em tabela:
        imprimir linha.email
    # enquanto: repete até a condição falhar.
    - tentativas = 0
    - enquanto tentativas < 3:
        tentativas = tentativas + 1
    - imprimir tentativas   # 3
    # tentar/capturar: o 'capturar' alinha com o '- tentar'; a variável
    # recebe a mensagem do erro (só erros T9xx de execução são capturáveis).
    - tentar:
        r = ler_csv "nao_existe.csv"
    capturar erro:
      registrar "falha esperada:", erro
```

- `para cada <var> em <lista|tabela>` — `<var>` aceita qualquer
  identificador, incluindo `e`/`ou`/`nao`/`contem` (não há palavras
  reservadas: o contexto sintático decide entre operador e nome).
- `enquanto` tem guarda de 5 milhões de iterações (aborta com `T901`).
- `tentar/capturar` captura `T9xx` de execução; a variável do `capturar` recebe a mensagem.
- `parar` sai do laço mais interno e `continuar` pula para a próxima iteração.
  Valem sozinhos na linha, dentro de `para cada`/`enquanto`; fora de laço o
  `tilt checar` acusa `T014`. (`parar = 1` continua sendo uma atribuição comum.)

```tilt run
funcao primeiro_par lista:
  para cada x em lista:
    se x % 2 == 0:
      retornar x
  retornar nulo

pipeline laco:
  passos:
    - soma = 0
    - para cada n em [1, 2, 3, 4, 5, 6]:
        se n % 2 == 0:
          continuar      # pula os pares
        se n > 5:
          parar          # sai do laço
        soma = soma + n
    - imprimir soma                       # 9  (1 + 3 + 5)
    - imprimir primeiro_par([1, 3, 8, 5]) # 8
```

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

- `valor se condicao senao outro` é o condicional em linha (associa à direita;
  só o ramo escolhido é avaliado). Tem a menor precedência: `(1 se c senao 2) + 10`
  pede parênteses. Sem o `senao`, o `tilt checar` acusa `T013`.
- `para cada i em 0..n:` percorre `0, 1, ..., n-1` (fim exclusivo, como o
  fatiamento `lista[0..2]`); equivale a `intervalo(0, n)`.
- `+` com texto concatena. `contem`: `"abcd" contem "bc"` ou `lista contem valor`.
- `e`/`ou` fazem curto-circuito no interpretador (não na VM — ver guia 09).
- `x?.campo` retorna `nulo` se `x` não tiver o campo, em vez de erro.

```tilt run
pipeline ops:
  passos:
    - imprimir "a" + "b"         # ab (texto concatena)
    - imprimir "abcd" contem "bc"  # verdadeiro
    - imprimir([1, 2] contem 2)    # verdadeiro (parênteses: sem ambiguidade)
    - imprimir nao falso           # verdadeiro
    - m = { nome: "ana" }
    - imprimir m?.idade            # nulo (campo ausente, sem erro)
```

## Coleções

```tilt run
pipeline colecoes:
  passos:
    - nomes = ["ana", "bruno", "caio"]
    - config = { epocas: 10, lote: 64 }
    - imprimir nomes[0]        # ana (índice começa em 0)
    - imprimir nomes[0..2]     # [ana, bruno] (fatia fim-exclusivo)
    - imprimir config.epocas   # 10 (campo de mapa)
```

> Listas e mapas literais podem ocupar várias linhas (as quebras de linha
> dentro de `[`/`{`/`(` são ignoradas); comentários `#` também podem aparecer
> dentro.

## Importar

```tilt run
importar io
importar rede

pipeline imports:
  passos:
    # 'io.juntar_caminhos' vem do módulo io.tilt da stdlib.
    - base = io.juntar_caminhos "/tmp", "a.json"
    - imprimir base   # /tmp/a.json
```

```tilt run
# 'de io importar X' traz o nome para o escopo principal.
de io importar existe_arquivo

pipeline imports2:
  passos:
    - imprimir existe_arquivo "/tmp"   # verdadeiro
```

> `importar io` carrega o arquivo `io.tilt` e expõe as `funcao` dele como
> `io.ler_json_seguro(...)`; `de io importar ler_json_seguro` traz o nome para
> o escopo principal. A busca é, nesta ordem: (1) `io.tilt` ao lado do
> arquivo que importa; (2) cada diretório de `TILT_STDLIB_PATH` (separados
> por `:`); (3) `stdlib/` ao lado do binário; (4)
> `<binário>/../share/tilt/stdlib` (layout da instalação). Erro claro (`T901`)
> lista onde foi procurado. A stdlib traz `io` (arquivos/caminhos),
> `rede` (HTTP JSON: `get_json`/`post_json`) e `nn` (camadas sobre tensor) —
> ver guia 04 e guia 12.
>
> Apelidos com `como`: `importar io como arquivos` (usa-se `arquivos.juntar_caminhos(...)`)
> e `de io importar existe_arquivo como existe` (traz o nome com outro nome).
> Com apelido, o `tilt checar` só reconhece o apelido.
