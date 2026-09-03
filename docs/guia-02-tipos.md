# 02 — Tipos

Tipos são **opcionais**: o compilador infere. Anotações servem para contratos
(`tipo`, `entrada:`, assinatura de `funcao`) e para as formas de tensor.

## `tipo` — registros

```tilt
tipo EntradaInferencia:
  texto: texto
  vetor_contexto: tensor[f32, 1536]
  temperatura: decimal = 0.2        # valor padrão
```

Campos: `nome: <tipo> [= <padrao>]`. Um `tipo` pode ser usado como anotação em
`entrada:`, `saida:`, parâmetros de `funcao` e como `formato:` de `perguntar`.

## Escalares

`texto` · `inteiro` · `decimal` · `logico` · `nulo` · `tabela`

Regras de valor: `inteiro + inteiro → inteiro`; qualquer operação com `decimal`
→ `decimal`; `/` sempre produz `decimal` no interpretador; `inteiro` amplia para
`decimal` numa atribuição anotada.

## Construtores

| Sintaxe | Significado |
|---|---|
| `lista[T]` | lista homogênea de `T` |
| `mapa[K, V]` | mapa de `K` para `V` |
| `opcional[T]` | `T` ou `nulo` — acesso exige `se` ou `?.` |
| `fluxo[T]` | sequência assíncrona de `T` (streaming) |
| `tensor[dtype, dim...]` | tensor n-dimensional f32 |

## `tensor[dtype, dim...]`

`dtype` ∈ `f32 f16 bf16 f64 i8 i16 i32 i64 u8 bool` (`T034` se desconhecido).
Dimensões: inteiros, ou `_` para uma dimensão simbólica (lote).

```tilt
tipo Lote:
  x: tensor[f32, 64, 1536]
  y: tensor[i32, 64]
```

Sufixo de dispositivo: `tensor[f32, 64, 1536] no dispositivo gpu`.

Em valores, tensores vêm de builtins (`tensor`, `zeros`, `uns`, `aleatorio`),
de `incorporar`, ou de `modelo X.executar` — ver [guia 04](guia-04-ml-dl.md).

## `tabela`

DataFrame colunar em memória: uma lista de mapas (linhas). Produzida por
`ler_csv`, `ler_json`, `carregador`, `ler <fonte>`, ou pelos métodos de tabela
(`filtrar`, `agrupar_por`, ...). Ver [guia 03](guia-03-dados.md).

## União de literais

```tilt
tipo Resumo:
  sentimento: "positivo" | "neutro" | "negativo"
```

Aceita qualquer `texto`; num `formato:` de LLM em modo `mock` o campo recebe o
primeiro literal da união.

## Inferência e verificação

`tilt checar` valida:

- declarações duplicadas (`T032`) — exceto `treino X` / `modelo X` de mesmo nome;
- tipos desconhecidos em campos de `tipo`, params/retorno de `funcao`, `entrada:`/`saida:` (`T033`);
- `tensor[...]` malformado — dtype ou dimensão inválidos (`T034`);
- segredo literal em `chave`/`token`/`senha`/`segredo`/`api_key` (`T020`);
- `dispositivo:` fora de `auto|cpu|gpu|metal|"cuda:N"` (`T021`);
- `ferramentas:` de `agente` apontando `ferramenta` não declarada (`T031`).

> Resolução de nomes dentro de `passos:` e o solver de dimensões de matmul
> ainda não fazem parte do `checar` (chegam numa próxima passada).
