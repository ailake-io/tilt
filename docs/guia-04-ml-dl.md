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
| `.dados` | lista plana dos valores (row-major) |
| `.matmul(outro)` | produto matricial 1D/2D |
| `.mais(outro)` | soma (alias de `+`) |
| `.transposta` | transposta 2D |
| `.reformar([d, d])` | reshape (mesmo número de elementos) |
| `.relu` `.gelu` `.silu` `.sigmoide` `.tanh` | ativação elementwise |
| `.softmax` | softmax no último eixo |
| `.conv2d(nucleo, passo: N)` | convolução 2D NCHW, padding válido (ver abaixo) |
| `.norma_lote(gama, beta, media, variancia, eps: e, em_treino: b)` | batch norm por canal (ver abaixo) |
| `.soma` `.media` | redução total → `decimal` |
| `.argmax` | índice do maior no último eixo |
| `.item` | escalar de um tensor de 1 elemento |

```tilt
- x = tensor [0.2, 0.5, 0.1, 0.9]
- imprimir x.forma, (x * 2).soma
- m = tensor [[1, 2], [3, 4]]
- imprimir m.matmul(m).forma
```

### Shape solver (`tilt checar`)

Com formas inteiramente literais ou anotadas, `checar` propaga e valida
dimensões antes de executar (`T012`). Formas entram por literais
(`tensor [...]`, `uns`/`zeros`/`aleatorio [..]`) e por anotações
(`entrada: tensor[...]`, parâmetros de `funcao` como `x: tensor[...]`) e
propagam por atribuição. O que é verificado:

| Operação | Verificação em `checar` |
|---|---|
| `.conv2d(nucleo, passo: N)` | entrada 4D `[N, C_in, H, W]`, núcleo 4D `[C_out, C_in, KH, KW]`, `C_in` coincidente, núcleo não maior que a entrada, `passo >= 1` — saída `[N, C_out, (H-KH)/passo+1, (W-KW)/passo+1]` |
| `.norma_lote(...)` | rank >= 2 (`[N, C, ...]`) — forma preservada |
| `.softmax`, ativações, `norma_camada` | forma preservada |
| `.reformar([d, d])` | mesmo número de elementos |
| `.transposta` | tensor 2D |
| `.matmul(outro)` | dimensão interna compatível (2D) |

O que o solver **não** deriva vira "forma desconhecida" e segue sem
verificação (o erro, se houver, continua vindo em runtime): formas através
de chamadas de `funcao`, condicionais, dimensões `_`/não literais, broadcast
parcial (viés `[N]` no último eixo) e pesos vindos de arquivo.

### Convolução 2D e batch norm

`conv2d` é uma operação de tensor (não uma camada de `modelo`): entrada
`[N, C_in, H, W]` convoluída com núcleo `[C_out, C_in, KH, KW]`, padding
**válido** (sem borda), `passo:` (stride) opcional default 1 — saída
`[N, C_out, (H-KH)/passo+1, (W-KW)/passo+1]`. Sem dilation nem padding
explícito por ora.

```tilt
- x = tensor [[[[1.0, 2.0, 3.0], [4.0, 5.0, 6.0], [7.0, 8.0, 9.0]]]]  # [1,1,3,3]
- k = tensor [[[[1.0, 0.0], [0.0, 1.0]]]]                            # [1,1,2,2]
- y = x.conv2d k                                                      # [1,1,2,2]
- imprimir y.dados                                                    # [6, 8, 12, 14]
- y2 = x.conv2d k, passo: 2                                           # stride 2
```

`norma_lote` normaliza por canal sobre `[N, C, ...]`:
`y = gama * (x - media) / sqrt(var + eps) + beta`. `gama`, `beta`, `media`
e `variancia` são tensores `[C]` (ou escalares, broadcast). Na inferência
passe `media`/`variancia` dos lotes de treino; com `em_treino: verdadeiro`
elas são calculadas do próprio lote (variância populacional) e podem ser
omitidas. `eps:` default `0.00001`.

```tilt
- xb = tensor [[[1.0, 4.0], [3.0, 8.0]]]     # [N=1, C=2, 2]
- nb = xb.norma_lote uns [2], zeros [2], tensor [2.0, 6.0], tensor [0.25, 4.0]
- imprimir nb.dados                           # [-2, 4, -1.5, 1]
- nt = xb.norma_lote uns [2], zeros [2], em_treino: verdadeiro
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
  pesos: "modelos/clf.pesos"         # carregados se existirem; ausente -> Xavier + nota
```

Camadas: `densa: N`, `linear: [entrada, saida]`, `ativacao: relu|gelu|silu|sigmoide|tanh`,
`softmax`, `abandono: p` / `dropout: p`, `norma_camada` (normalização sobre a
última dimensão, sem affine — na inferência e no treino). `norma_lote`/`conv2d`
não são camadas de `modelo` — existem como operações de tensor (ver acima) e
no `modelo` produzem erro claro em vez de serem ignoradas silenciosamente.

### Pesos de arquivo

`pesos: "caminho"` carrega pesos no formato **tilt-pesos** (JSON gerado por
`modelo <Nome>.salvar_pesos`, com `w`/`b` por camada densa). Forma
incompatível com o modelo → erro `T901` mostrando o esperado vs. o encontrado;
arquivo ausente → init Xavier com `[nota]`.

```tilt
- modelo Classificador.salvar_pesos "modelos/clf.pesos"
```

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
  # perda: quadratica              # regressão escalar: saída largura 1, sem softmax
  otimizador: adam                  # sgd | adam
  taxa: 0.05                         # ou taxa_aprendizado:
  epocas: 150
  verboso: verdadeiro               # imprime a perda a cada ~epocas/10
```

Perdas: `entropia_cruzada` (classificação, exige `softmax` final) e
`quadratica` (regressão escalar — a saída deve ter largura 1 e não ter
`softmax`). O backward cobre todas as camadas: `densa`/`linear` (com SGD/Adam),
ativações (derivada exata da mesma aproximação da forward — inclusive `gelu`)
e `norma_camada` (sem affine). Resumo determinístico:

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
