# 03 — Engenharia de dados

## `fonte` — conector de arquivo local

```tilt
fonte produtos:
  tipo: json                       # csv | json  (postgres/kafka/s3 -> T900)
  caminho: "dados/produtos.json"   # ou arquivo: / url:  ; aceita  env "VAR"
```

Num pipeline, `ler produtos` lê o arquivo conforme `tipo:` e devolve uma
`tabela`. `file://` é removido do caminho. Conectores de rede (`postgres`,
`kafka`, `s3`, `delta`) levantam `T900` apontando o marco.

## `pipeline`

```tilt
pipeline etl:
  agenda: "0 * * * *"              # cron de 5 campos (validado; loop real: futuro)
  ao_falhar: repetir 3             # reexecuta os passos ate 3x com escopo limpo
  passos:
    - bruto = ler_csv "clientes.csv"
    - limpo = bruto.filtrar linha.email contem "@"
    - agg = limpo.agrupar_por "dominio", { total: contar, receita: somar "valor" }
    - escrever_csv agg, "saida.csv"
```

`tilt executar` roda **todo** `pipeline` de topo, na ordem do arquivo. Sem
pipeline, roda uma `funcao principal` se existir.

`tilt executar --agendar` valida o `agenda:` e reconhece o modo (a execução em
loop chega numa próxima passada).

## Leitura e escrita

| Builtin | Efeito |
|---|---|
| `ler_csv "caminho"` | → `tabela` (1ª linha = cabeçalho; célula vira inteiro/decimal/texto) |
| `ler_json "caminho"` | array de objetos → `tabela`; objeto → `mapa` |
| `escrever_csv <tabela>, "caminho"` | grava CSV |
| `escrever_json <valor>, "caminho"` | grava JSON pretty (chaves em ordem de inserção) |
| `escrever_parquet <tabela>, "caminho"` | grava CSV com aviso (Parquet: futuro) |
| `ler <fonte>` | lê a `fonte` declarada |
| `carregador "d.csv", alvo: "col"` | → `{ x: tensor[N,F], y: lista, atributos: lista }` |

## Métodos de tabela

Operam sobre `tabela` e `lista` de mapas. `linha` é a variável implícita da linha atual.

| Método | Efeito |
|---|---|
| `.filtrar <cond>` | mantém as linhas onde `<cond>` (com `linha`) é verdadeira |
| `.derivar { col: <expr> }` | adiciona/atualiza colunas por linha |
| `.mapear { col: <expr> }` | idem `.derivar` |
| `.selecionar "a", "b"` | mantém só as colunas nomeadas |
| `.agrupar_por "col", { nome: <agg> }` | agrupa; `<agg>` ∈ `contar`, `somar "c"`, `media "c"`, `min "c"`, `max "c"` |
| `.ordenar_por "col", desc: verdadeiro` | ordena (numérico ou lexicográfico) |
| `.limite N` / `.primeiros N` | primeiras N linhas |
| `.distinto` / `.distinto "col"` | remove duplicatas |
| `.tamanho` | número de linhas |

```tilt
- vendas = ler_csv "vendas.csv"
- top = vendas.filtrar linha.valor >= 50
             .agrupar_por "regiao", { receita: somar "valor", n: contar }
             .ordenar_por "receita", desc: verdadeiro
             .limite 3
- para cada r em top:
    imprimir r.regiao, r.receita, r.n
```

## `verificar` — qualidade de dados

Passo dentro de `passos:` que valida uma tabela já no escopo:

```tilt
- verificar vendas:
    - nao_nulo: [regiao, valor]        # coluna ausente ou nula
    - unico: id                         # valor repetido
    - intervalo: linha.valor >= 0       # condição avaliada por linha
    ao_violar: abortar                  # abortar (T910) | avisar (segue)
```

Com `ao_violar: avisar` imprime `[aviso] verificar ...` e continua; com
`abortar` (padrão) lança `T910` com as primeiras violações como notas.

## Builtins de apoio

`tamanho` · `contar` · `somar`/`media`/`min`/`max` (sobre lista de números) ·
`intervalo n` / `intervalo a, b` (→ lista de inteiros) · `dividir "texto", "sep"`
(→ lista) · `dividir_texto "texto", tamanho: N, sobreposicao: M` (janela deslizante) ·
`imprimir` · `registrar` · `env "VAR"`.
