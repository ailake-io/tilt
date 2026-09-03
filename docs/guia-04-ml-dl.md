# 04 — ML e Deep Learning

## Tensores

f32, row-major, na CPU (kernels escalares; GPU opcional adiante).

| Construtor | Resultado |
|---|---|
| `tensor [1, 2, 3]` | forma `[3]` |
| `tensor [[1, 2], [3, 4]]` | forma `[2, 2]` |
| `zeros [2, 3]` / `uns [2, 3]` | preenchido com 0 / 1 |
| `aleatorio [2, 3]` / `aleatorio [2, 3], semente: 7` | Xavier-uniforme determinístico |
| `incorporar "modelo", "texto"` | vetor de embedding, forma `[D]` |

`tensor [ ... ]` é açúcar (não colide com o *tipo* `tensor[f32, N]`, que só
aparece em anotação).

### Operadores e métodos

`+ - * /` funcionam entre tensores (elementwise, com broadcast de escalar e de
viés `[N]` no último eixo) e entre tensor e escalar.

| Método / propriedade | Efeito |
|---|---|
| `.forma` | lista das dimensões |
| `.matmul(outro)` | produto matricial 1D/2D |
| `.mais(outro)` | soma (alias de `+`) |
| `.transposta` | transposta 2D |
| `.reformar [d, d]` | reshape (mesmo número de elementos) |
| `.relu` `.gelu` `.silu` `.sigmoide` `.tanh` | ativação elementwise |
| `.softmax` | softmax no último eixo |
| `.soma` `.media` | redução total → `decimal` |
| `.argmax` | índice do maior no último eixo |
| `.item` | escalar de um tensor de 1 elemento |

```tilt
- x = tensor [0.2, 0.5, 0.1, 0.9]
- imprimir x.forma, (x * 2).soma
- m = tensor [[1, 2], [3, 4]]
- imprimir m.matmul(m).forma
```

## `modelo`

```tilt
modelo Classificador:
  dispositivo: auto                 # auto | cpu | gpu | "cuda:N"
  camadas:
    - linear: [4, 8]                 # ou  densa: 8  (infere a entrada)
    - ativacao: relu
    - abandono: 0.1                  # identidade na inferência
    - linear: [8, 3]
    - softmax
  pesos: "modelos/clf.pesos"         # carga de arquivo: futuro; hoje usa init Xavier
```

Camadas: `densa: N`, `linear: [entrada, saida]`, `ativacao: relu|gelu|silu|sigmoide|tanh`,
`softmax`, `abandono: p` / `dropout: p`. `norma_lote`/`norma_camada`/`conv2d`
são ignoradas com nota (chegam adiante).

### Inferência

```tilt
- entrada = tensor [0.2, 0.5, 0.1, 0.9]
- probs = modelo Classificador.executar entrada        # ou .para_frente
- imprimir probs.forma, probs.argmax
- lote = tensor [[0.1, 0.2, 0.3, 0.4], [0.9, 0.8, 0.7, 0.6]]
- imprimir modelo Classificador.executar(lote).forma   # [2, 3]
```

Init dos pesos: Xavier-uniforme com semente fixa → resultados reproduzíveis
sem arquivo de pesos.

## `treino`

`treino X` treina o `modelo X` de mesmo nome.

```tilt
treino Classificador:
  dados: carregador "flores.csv", alvo: "especie"
  perda: entropia_cruzada           # exige `softmax` na última camada
  otimizador: adam                  # sgd | adam
  taxa: 0.05                         # ou taxa_aprendizado:
  epocas: 150
  verboso: verdadeiro               # imprime a perda a cada ~epocas/10
```

Backprop escrito à mão para a pilha densa + ativação (`relu`/`sigmoide`/`tanh`/`silu`)
+ softmax. Resumo determinístico:

```
treino Classificador: perda caiu sim | acuracia 90/90
```

Pós-treino os pesos ficam no `modelo` — chamadas seguintes de
`modelo Classificador.executar` usam o modelo treinado.

## GPU

`dispositivo: auto|gpu|"cuda:N"` + `TILT_GPU`:

| `TILT_GPU` | Comportamento |
|---|---|
| `off` (padrão) | CPU sempre |
| `auto` | tenta `dlopen` de `libcuda` + `libnvrtc`, compila os kernels; sem driver → CPU (silencioso) |
| `fake` | roteia `matmul`/`relu` pelo caminho de dispatch da GPU usando math de CPU — saída idêntica, útil para testar sem hardware |

Quando um backend de GPU ativa, imprime uma vez `[gpu] <info>`. O caminho CUDA
foi validado apenas em hardware.
