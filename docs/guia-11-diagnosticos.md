# 11 — Diagnósticos

Todo diagnóstico tem um código estável `Tnnn`, local exato, trecho da fonte,
`esperado` vs `encontrado` quando aplicável, e uma sugestão. `tilt checar --json`
entrega tudo em JSON.

## Lexer — `T001`–`T004`

| Código | Significado | Causa comum |
|---|---|---|
| `T001` | indentação não é múltiplo de 2 | 3 espaços em vez de 2 ou 4 |
| `T002` | tab na indentação | tecla Tab; troque por 2 espaços |
| `T003` | texto não terminado | falta a aspa de fechamento na mesma linha, ou `"""` sem fim |
| `T004` | caractere inválido | `!` sozinho (use `nao` ou `!=`), `?` sozinho (use `?.`) |

## Parser — `T010`, `T013`, `T014`

| Código | Significado |
|---|---|
| `T010` | indentação inesperada — nenhum bloco aberto nesse nível |
| `T013` | token esperado — falta `:`, `]`, `}`, `->` ... |
| `T014` | token inesperado — expressão ou declaração fora de lugar; `e`/`ou`/`nao`/`contem` como nome de variável |

## Semântica — `T011`, `T012`, `T020`–`T034`

| Código | Significado |
|---|---|
| `T011` | tipo incompatível com a anotação |
| `T012` | forma de tensor incompatível (solver completo: futuro) |
| `T020` | segredo literal em `chave`/`token`/`senha`/`segredo`/`api_key` — use `env "VAR"` |
| `T021` | `dispositivo:` fora de `auto`, `cpu`, `gpu`, `metal`, `"cuda:N"` |
| `T030` | nome não definido (checagem em `passos:`: futuro) |
| `T031` | `ferramentas:` de `agente` referencia `ferramenta` não declarada |
| `T032` | declaração duplicada — aponta a linha da anterior (exceto `treino X`/`modelo X`) |
| `T033` | tipo desconhecido em campo de `tipo`, param/retorno de `funcao`, `entrada:`/`saida:` |
| `T034` | anotação de tensor malformada — dtype inválido ou dimensão que não é inteiro/`_` |

## Execução — `T9nn`

| Código | Significado |
|---|---|
| `T900` | conector não implementado (`postgres`, `kafka`, `s3`, `parquet`, `qdrant`, ...) |
| `T901` | erro de execução (arquivo ausente, índice fora dos limites, laço estourou, LLM falhou, ...) |
| `T902` | recurso não implementado (LLM sem `TILT_LLM=mock` e sem `curl`, método de modelo futuro, ...) |
| `T910` | violação de qualidade de dados (`verificar` com `ao_violar: abortar`) |

## Exemplos

```
erro[T034]: dtype de tensor desconhecido 'f33'
  --> prog.tilt:2:13
   |
 2 |   v: tensor[f33, 1536]
   |             ^^^
   |
   = use f32, f16, bf16, f64, i8, i32, i64 ou u8

erro[T020]: segredo em 'chave' nao deve ser um literal
  --> prog.tilt:3:10
   |
 3 |   chave: "sk-proj-1234567890"
   |          ^^^^^^^^^^^^^^^^^^^^
   |
   = use chave: env "NOME_DA_VARIAVEL"
```
