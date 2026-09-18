# 02 — Tipos

Tipos são **opcionais**: o compilador infere. Anotações servem para contratos
(`tipo`, `entrada:`, assinatura de `funcao`) e para as formas de tensor.

## `tipo` — registros

```tilt run
# Registro com 3 campos tipados; 'tipo' so declara, nao executa nada.
tipo EntradaInferencia:
  texto: texto
  vetor_contexto: tensor[f32, 1536]
  temperatura: decimal

pipeline tipos:
  passos:
    - imprimir "tipo declarado"
```

Campos: `nome: <tipo>`, com valor padrão opcional `nome: <tipo> = <valor>`.
O padrão precisa ser compatível com o declarado (`T011` caso contrário) e é
aplicado quando o campo falta: em `formato:` de `perguntar` (inclusive no mock)
e em `entrada:` de rotas (sem 400). Um `tipo` pode ser usado como anotação
em `entrada:`, parâmetros de `funcao` e como `formato:` de `perguntar`.

```tilt run
tipo Pedido:
  nome: texto = "anon"
  qtd: inteiro = 1

pipeline p:
  passos:
    - imprimir "tipo com padrao"
```

```tilt run
# 'entrada:' valida a rota contra o tipo (campo faltando ou com tipo
# errado vira erro antes dos passos).
tipo Pedido:
  cliente: texto
  valor: decimal

servico Loja:
  porta: 8080
  rota post "/pedidos":
    entrada: Pedido
    passos:
      - responder:
          dados:
            total: entrada.valor * 1.1
```

## Escalares

`texto` · `inteiro` · `decimal` · `logico` · `nulo` · `tabela`

Regras de valor: `inteiro + inteiro → inteiro`; qualquer operação com `decimal`
→ `decimal`; `/` sempre produz `decimal` no interpretador; `inteiro` amplia para
`decimal` numa atribuição anotada.

```tilt run
pipeline escalares:
  passos:
    - imprimir 2 + 3       # 5 (inteiro + inteiro -> inteiro)
    - imprimir 2 + 3.0     # 5 (decimal contamina)
    - imprimir 5 / 2       # 2.5 (/ sempre dá decimal)
    - imprimir 2 < 3       # verdadeiro
```

## Construtores

| Sintaxe | Significado |
|---|---|
| `lista[T]` | lista homogênea de `T` |
| `mapa[K, V]` | mapa de `K` para `V` |
| `opcional[T]` | `T` ou `nulo` — acesso exige `se` ou `?.` |
| `fluxo[T]` | sequência assíncrona de `streaming` (tokens, SSE, Kafka) |
| `tensor[dtype, dim...]` | tensor n-dimensional f32 |

## `tensor[dtype, dim...]`

`dtype` ∈ `f32 f16 bf16 f64 i8 i16 i32 i64 u8 bool` (`T034` se desconhecido).
Dimensões: inteiros, ou `_` para uma dimensão simbólica (lote).

```tilt run
tipo Lote:
  x: tensor[f32, 64, 1536]
  y: tensor[i32, 64]

pipeline tensores:
  passos:
    - t = tensor [1, 2, 3]   # tensor 1D de 3 elementos
    - imprimir t.forma        # [3]
    - imprimir t.soma         # 6
```

Sufixo de dispositivo: `tensor[f32, 64, 1536] no dispositivo gpu`.

Em valores, tensores vêm de builtins (`tensor`, `zeros`, `uns`, `aleatorio`),
de `incorporar`, ou de `modelo X.executar` — ver [guia 04](guia-04-ml-dl.md).

## `tabela`

DataFrame colunar em memória: uma lista de mapas (linhas). Produzida por
`ler_csv`, `ler_json`, `carregador`, `ler <fonte>`, ou pelos métodos de tabela
(`filtrar`, `agrupar_por`, ...). Ver [guia 03](guia-03-dados.md).

```tilt run
pipeline tabela:
  passos:
    # Lista de mapas vira tabela ao passar por método de tabela.
    - vendas = [{ regiao: "sul", valor: 10 }, { regiao: "norte", valor: 20 }]
    - total = vendas.agrupar_por "regiao", { receita: somar "valor" }
    - para cada r em total:
        imprimir r.regiao, r.receita
```

## União de literais

```tilt run
# A união restringe os valores aceitos (texto fora dela falha no checar
# quando o tipo é conhecido; no LLM, ver guia 05).
tipo Resumo:
  sentimento: "positivo" | "neutro" | "negativo"

pipeline uniao:
  passos:
    - imprimir "uniao declarada"
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
- `ferramentas:` de `agente` apontando `ferramenta` não declarada (`T031`);
- nomes dentro de `passos:` / `executar:` (`T030`) — escopo global mais
  variáveis implícitas (`linha`, `entrada`, `epoca`, `metricas`, `passo`,
  `resultado`) e campos de `entrada:`;
- dimensões da cadeia de camadas `densa`/`linear` de `modelo` (`T012`) —
  propaga a dimensão corrente a partir de `entrada: tensor[...]`.

> O solver de formas cobre `conv2d`/`norma_lote`/`reformar`/`transposta`/
> `matmul` com formas literais ou anotadas (ver guia 04), e `tilt checar`
> agora infere e verifica tipos entre expressões (`T011`): operadores
> aritméticos/comparação, builtins (aridade e 1º/2º argumentos), métodos e
> campos de receiver conhecido, fluxo condicional com definições em todos os
> ramos (incluindo promoção `inteiro` → `decimal`) e o retorno de `funcao` anotada. Ainda não há
> unificação de tipos genericos nem checagem de `verificar`/`ao_falhar`.
