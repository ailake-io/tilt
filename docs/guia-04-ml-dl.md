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
| `.conv2d(nucleo, passo: N, padding: P, dilatacao: D)` | convolução 2D NCHW com padding e dilatação (ver abaixo) |
| `.norma_lote(gama, beta, media, variancia, eps: e, em_treino: b)` | batch norm por canal (ver abaixo) |
| `.norma_camada()` | layer norm no último eixo (sem affine) |
| `.soma` `.media` | redução total → `decimal` |
| `.argmax` | índice do maior no último eixo |
| `.item` | escalar de um tensor de 1 elemento |

```tilt run
pipeline tensores:
  passos:
    - x = tensor [0.2, 0.5, 0.1, 0.9]
    - imprimir x.forma, (x * 2).soma   # [4] 4.4
    - m = tensor [[1, 2], [3, 4]]
    - imprimir m.matmul(m).forma        # [2, 2]
```

### Shape solver (`tilt checar`)

Com formas inteiramente literais ou anotadas, `checar` propaga e valida
dimensões antes de executar (`T012`). Formas entram por literais
(`tensor [...]`, `uns`/`zeros`/`aleatorio [..]`) e por anotações
(`entrada: tensor[...]`, parâmetros de `funcao` como `x: tensor[...]`) e
propagam por atribuição. Funções locais com entrada e retorno tensor anotados
também carregam a forma para o chamador; dimensões `_` do retorno são
instanciadas pelas dimensões conhecidas dos argumentos. Atribuições diretas a
campos de mapa (`m.campo = tensor`) preservam a forma para os passos seguintes.
O que é verificado:

| Operação | Verificação em `checar` |
|---|---|
| `.conv2d(nucleo, passo: N, padding: P, dilatacao: D)` | entrada 4D, núcleo 4D, canais coincidentes, `passo >= 1`, `padding >= 0`, `dilatacao >= 1`; saída usa o kernel efetivo `(K-1)*D+1` |
| `.norma_lote(...)` | rank >= 2 (`[N, C, ...]`) — forma preservada |
| `.softmax`, ativações, `norma_camada` | forma preservada |
| `.reformar([d, d])` | mesmo número de elementos |
| `.transposta` | tensor 2D |
| `.matmul(outro)` | dimensão interna compatível (2D) |

O que o solver **não** deriva vira "forma desconhecida" e segue sem
verificação (o erro, se houver, continua vindo em runtime): formas através
de funções genéricas sem contrato tensor, condicionais, dimensões não
literais, broadcast parcial (viés `[N]` no último eixo) e pesos vindos de
arquivo.

### Convolução 2D e batch norm

`conv2d` existe como operação de tensor e como camada de `modelo`:
entrada `[N, C_in, H, W]` convoluída com núcleo `[C_out, C_in, KH, KW]`,
com padding e dilatação simétricos — saída
`[N, C_out, floor((H + 2*P - KH_eff)/passo)+1, floor((W + 2*P - KW_eff)/passo)+1]`,
onde `KH_eff = (KH-1)*D+1` e `KW_eff = (KW-1)*D+1`.

```tilt run
pipeline conv:
  passos:
    - x = tensor [[[[1.0, 2.0, 3.0], [4.0, 5.0, 6.0], [7.0, 8.0, 9.0]]]]  # [1,1,3,3]
    - k = tensor [[[[1.0, 0.0], [0.0, 1.0]]]]                            # [1,1,2,2]
    - y = x.conv2d k                                                      # [1,1,2,2]
    - imprimir y.dados                                                    # [6, 8, 12, 14]
    - y2 = x.conv2d k, passo: 2                                           # stride 2 -> [1,1,1,1]
    - imprimir y2.forma
```

`norma_lote` normaliza por canal sobre `[N, C, ...]`:
`y = gama * (x - media) / sqrt(var + eps) + beta`. `gama`, `beta`, `media`
e `variancia` são tensores `[C]` (ou escalares, broadcast). Na inferência
passe `media`/`variancia` dos lotes de treino; com `em_treino: verdadeiro`
elas são calculadas do próprio lote (variância populacional) e podem ser
omitidas. `eps:` default `0.00001`.

```tilt run
pipeline normab:
  passos:
    - xb = tensor [[[1.0, 4.0], [3.0, 8.0]]]     # [N=1, C=2, 2]
    - nb = xb.norma_lote uns [2], zeros [2], tensor [2.0, 6.0], tensor [0.25, 4.0]
    - imprimir nb.dados                           # [-2, 4, -1.5, 1]
    - nt = xb.norma_lote uns [2], zeros [2], em_treino: verdadeiro
    - imprimir nt.forma                           # [1, 2, 2]
```

## `experimento` (ML clássico)

`experimento` ajusta um modelo clássico sobre uma tabela e já imprime as
métricas no teste. Roda antes dos `pipeline`s (como `treino`), então
`prever` funciona em qualquer passo, rota ou outro experimento.

```tilt run
experimento prever_churn:
  dados: [
    { uso: 10, plano: "a", churn: 1 },
    { uso: 9, plano: "b", churn: 1 },
    { uso: 2, plano: "a", churn: 0 },
    { uso: 1, plano: "b", churn: 0 }
  ]
  alvo: "churn"
  atributos: [uso, plano]        # default: todas as colunas menos o alvo
  pre_processar:
    - um_de_n: [plano]           # one-hot (ordem de aparição no treino)
    - padronizar: [uso]          # média/desvio do treino
  dividir: { treino: 0.75, teste: 0.25 }   # default; validacao: opcional
  modelo: regressao_logistica    # + regressao_linear | knn | kmeans |
                                 #   floresta_aleatoria | gradiente_impulsionado | svm
    taxa: 0.5                    # logistica (GD em lote; default 0.5/500)
    epocas: 500
  validacao_cruzada: 4           # opcional; media +- desvio no final
  metricas: [acuracia, f1, auc, matriz_confusao]  # default por tarefa
  semente: 7                     # embaralhamento e kmeans (default 42)

pipeline usa:
  passos:
    - p = experimento prever_churn.prever { uso: 9, plano: "a" }
    - imprimir p.classe, p.probabilidade
```

Modelos: `regressao_linear` (equações normais + crista 1e-8; prevê
`{valor}`), `regressao_logistica` (binária, GD interno com padronização
própria; prevê `{classe, probabilidade}` = P da classe prevista), `knn`
(`vizinhos:`, voto majoritário ou média; distância euclidiana em atributos
padronizados internamente) e `kmeans` (`grupos:` obrigatório, sem `alvo:` nem
`metricas:`; prevê `{grupo}`, reporta `inercia` + tamanhos),
`floresta_aleatoria` (`arvores:`, `profundidade:`), `gradiente_impulsionado`
(`arvores:`, `taxa:`, `profundidade:`) e `svm` (`custo:`, binária). Alvo
numérico binário (2 valores) classifica; com 3+ valores, floresta/GBM fazem
regressão. Nulos com `- imputar: [cols]` (média/moda do treino).

Métricas: classificação `acuracia` (default), `f1` (ponderado pelo suporte),
`auc` (binária; exige exemplos das 2 classes no teste) e `matriz_confusao`;
regressão `rmse` (default) e `r2`. `registrar_em: "mlflow://host/experimento"`
usa o Tracking REST do MLflow: localiza/cria o experimento, cria um run, envia
parâmetros e métricas em `log-batch` e finaliza o run. Para servidores
protegidos, `MLFLOW_TRACKING_TOKEN` envia Bearer e `MLFLOW_WORKSPACE` envia
o workspace. `dados:` aceita tabela inline, caminho `.csv`/`.parquet`/`.json`
ou o valor de `ler_*`.

## `modelo`

```tilt run
modelo Classificador:
  dispositivo: auto                 # auto | cpu | gpu | "cuda:N"
  camadas:
    - linear: [4, 8]                 # ou  densa: 8  (infere a entrada)
    - ativacao: relu
    - abandono: 0.1                  # identidade na inferência
    - linear: [8, 3]
    - softmax

pipeline classifica:
  passos:
    # Vetor de 4 uns atravessa a rede e sai com 3 classes.
    - entrada = uns [4]
    - probs = modelo Classificador.executar entrada
    - imprimir probs.forma         # [3]
    - imprimir probs.argmax
```

Camadas: `densa: N`, `linear: [entrada, saida]`, `ativacao: relu|gelu|silu|sigmoide|tanh`,
`softmax`, `abandono: p` / `dropout: p`, `norma_camada` (normalização sobre a
última dimensão, sem affine — na inferência e no treino),
`conv2d: [C_saida, C_entrada, KH, KW, passo, padding, dilatacao]` (os três últimos são opcionais),
`norma_lote` (affine por canal, com média/variância correntes),
`incorporacao: [vocabulario, dimensao]` (indices inteiros; adiciona a dimensao D ao final; por exemplo entrada `[N, T]` vira `[N, T, D]`),
`recorrente: [rnn|lstm|gru, oculta]` (sequencias `[tempo, atributos]` ou lotes `[N, tempo, atributos]`; retorna o ultimo estado),
`agrupamento_max: [janela]` ou `[janela, passo]` e `achatar` (achata o lote
`[N, ...]` para `[N, C]` antes da `densa`). Modelos convolucionais exigem a
anotação completa da entrada, ex.: `entrada: tensor[f32, 1, 4, 4]` (sem o
lote). No `treino`, `x` pode ser 3D `[N, T, F]` para recorrentes ou 4D `[N, C, H, W]` quando o modelo começa
com `conv2d`.

```tilt run
modelo CNN:
  entrada: tensor[f32, 1, 4, 4]
  camadas:
    - conv2d: [2, 1, 2, 2]
    - ativacao: relu
    - norma_lote
    - agrupamento_max: [2]
    - achatar
    - linear: [2, 2]
    - softmax

pipeline cnn:
  passos:
    - y = modelo CNN.executar uns [1, 1, 4, 4]
    - imprimir y.forma, y.argmax   # [1, 2] <classe>
```

### Pesos de arquivo

`pesos: "caminho"` carrega pesos no formato **tilt-pesos** (JSON) ou **Safetensors** (F32 binário). O JSON é gerado por
`modelo <Nome>.salvar_pesos`, com `w`/`b` por camada com parâmetros:
`densa`/`linear`, `conv2d` — incluindo `passo` — e `norma_lote` — incluindo
`media_running`/`var_running`). Forma
incompatível com o modelo → erro `T901` mostrando o esperado vs. o encontrado;
arquivo ausente → init Xavier com `[nota]`. O carregamento também pode ser
feito em tempo de execução com `modelo <Nome>.carregar_pesos "caminho"`
(mesma validação de formas; o modelo passa a usar os pesos carregados nas
chamadas seguintes de `executar`). Safetensors também pode ser salvo/carregado com a extensão `.safetensors`; ele usa tensores nomeados `camada_<i>.w`, `.b` e `.u` para recorrentes.

```tilt run
modelo Mini:
  entrada: tensor[f32, 2]
  camadas:
    - densa: 2
    - softmax

pipeline pesos:
  passos:
    # Salva no formato tilt-pesos (JSON); 'pesos:' do modelo carrega de volta.
    - modelo Mini.salvar_pesos "mini.pesos"
    - modelo Mini.carregar_pesos "mini.pesos"
    - imprimir "ok"
```

### Exportação ONNX

`modelo <Nome>.exportar_onnx "modelo.onnx"` exporta o modelo (pesos atuais,
incluindo pós-`treino`) para **ONNX opset 20**, sem dependências externas:
cada `densa`/`linear` vira um `Gemm`, ativações viram `Relu`/`Gelu`/
`Sigmoid`+`Mul` (`silu`) /`Sigmoid`/`Tanh`, mais `Softmax` (eixo 1),
`LayerNormalization`, `Conv`, `BatchNormalization`, `MaxPool`, `Flatten` e `RNN`/`LSTM`/`GRU`;
`abandono` é identidade na inferência e não é
exportado. A entrada é `[lote, ...]` (`lote` dinâmico, resto de
`entrada: tensor[...]`). O arquivo passa no `onnx.checker` e roda em
qualquer runtime ONNX (ex.: onnxruntime). `gelu` usa a aproximação tanh da
Tilt, então pode diferir ~1e-4 do `Gelu` exato do ONNX.

```tilt run
modelo Mini:
  entrada: tensor[f32, 2]
  camadas:
    - densa: 4
    - ativacao: relu
    - densa: 2
    - softmax

pipeline exporta:
  passos:
    - modelo Mini.exportar_onnx "mini.onnx"
    - modelo Mini.exportar_gguf "mini.gguf"
    - imprimir "ok"
```

### Exportação GGUF

`modelo <Nome>.exportar_gguf "modelo.gguf"` grava os pesos no formato
**GGUF v3** (o mesmo do llama.cpp), sem dependências: cada camada com
parâmetros vira dois tensores F32 (`camada-<i>.peso` e `camada-<i>.vies`,
dimensões invertidas por convenção do GGUF) mais metadados
(`general.architecture = "tilt"`, nome do modelo). Só escrita — a Tilt não
executa GGUF (use llama.cpp/ollama para inferir).

### Inferência

```tilt run
modelo Mini2:
  entrada: tensor[f32, 4]
  camadas:
    - densa: 3
    - softmax

pipeline infere:
  passos:
    - entrada = tensor [0.2, 0.5, 0.1, 0.9]
    - probs = modelo Mini2.executar entrada        # ou .para_frente
    - imprimir probs.forma, probs.argmax           # [3] <classe>
    - lote = tensor [[0.1, 0.2, 0.3, 0.4], [0.9, 0.8, 0.7, 0.6]]
    - lote_probs = modelo Mini2.executar lote
    - imprimir lote_probs.forma   # [2, 3]
```

Init dos pesos: Xavier-uniforme com semente fixa → resultados reproduzíveis
sem arquivo de pesos.

## `treino`

`treino X` treina o `modelo X` de mesmo nome.

```tilt run
# XOR em 4 amostras, tudo inline (sem arquivos).
modelo Xor:
  entrada: tensor[f32, 2]
  camadas:
    - densa: 2
    - softmax

treino Xor:
  dados: { x: [[0, 0], [0, 1], [1, 0], [1, 1]], y: [0, 1, 1, 0] }
  perda: entropia_cruzada           # exige `softmax` na última camada
  # perda: quadratica              # regressão escalar: saída largura 1, sem softmax
  otimizador: adam                  # sgd | adam
  taxa: 0.05                         # ou taxa_aprendizado:
  epocas: 3
  lote: 4                            # mini-lote (default: lote cheio); embaralha por época
  semente: 7                         # init Xavier + embaralhamento (reproduzível)
  embaralhar: verdadeiro              # falso preserva a ordem dos dados
```

Perdas: `entropia_cruzada` (classificação, exige `softmax` final) e
`quadratica` (regressão escalar — a saída deve ter largura 1 e não ter
`softmax`). O backward cobre todas as camadas: `densa`/`linear` (com SGD/Adam),
ativações (derivada exata da mesma aproximação da forward — inclusive `gelu`),
`norma_camada` (sem affine), `conv2d` (núcleo + viés, com SGD/Adam),
`incorporacao` (tabela treinável por SGD/Adam, com entrada de índices inteiros),
`norma_lote` (gama/beta, com estatísticas do lote no treino e média/variância
correntes na inferência), `agrupamento_max` e `achatar`. Resumo determinístico:

```
treino Xor: perda caiu sim | acuracia 2/4
```

Pós-treino os pesos ficam no `modelo` — chamadas seguintes de `modelo Xor.executar` usam o modelo treinado. (`verboso: verdadeiro`
imprime a perda a cada ~epocas/10.)

O bloco opcional `ao_epoca:` roda ao final de cada época concluída, inclusive antes
de uma parada antecipada, com as variáveis `epoca`, `modelo`, `perda`, `taxa` e
`perda_validacao` (nulo quando `validacao:` não foi configurada):

```tilt run
modelo Xor:
  entrada: tensor[f32, 2]
  camadas:
    - densa: 2
    - softmax

treino Xor:
  dados: { x: [[0, 0], [1, 1]], y: [0, 1] }
  epocas: 2
  ao_epoca:
    - imprimir "época", epoca, "perda", perda
```

### Checkpoint e retomada

`checkpoint: "caminho"` salva ao final (e a cada `a_cada: N` épocas) um
JSON **tilt-checkpoint** com pesos, momentos do Adam, estatísticas do
`norma_lote`, época e otimizador. `retomar: "caminho"` continua de onde
parou — bit a bit idêntico ao treino contínuo (mesmos `epocas:`,
`lote:`, `semente:`, `taxa:` e `otimizador:`; `epocas:` deve passar a
época do checkpoint):

```tilt run
modelo Xor:
  camadas:
    - densa: 2
    - softmax

treino Xor:
  dados: { x: [[0, 0], [0, 1], [1, 0], [1, 1]], y: [0, 1, 1, 0] }
  perda: entropia_cruzada
  otimizador: sgd
  epocas: 2
  checkpoint: "xor.json"

treino Xor:
  dados: { x: [[0, 0], [0, 1], [1, 0], [1, 1]], y: [0, 1, 1, 0] }
  perda: entropia_cruzada
  otimizador: sgd
  epocas: 4
  retomar: "xor.json"
```

### Agendador, validação e parada antecipada

```tilt run
modelo Xor:
  camadas:
    - densa: 2
    - softmax

treino Xor:
  dados: { x: [[0, 0], [0, 1], [1, 0], [1, 1]], y: [0, 1, 1, 0] }
  perda: entropia_cruzada
  otimizador: sgd
  epocas: 100
  agendador: { tipo: cosseno }      # ou { tipo: degrau, a_cada: 10, fator: 0.5 }
  validacao: 0.25                    # fração separada do treino (embaralhada)
  parar_cedo: { paciencia: 5 }       # ou N direto; + melhorar_min: D opcional
```

`agendador:` varia a taxa por época (cosseno até ~0, ou degrau que
multiplica por `fator` a cada `a_cada` épocas). `validacao:` reserva uma
fração para medir a perda em modo inferência a cada época; `parar_cedo:`
restaura os melhores pesos e interrompe após `paciencia` épocas sem melhora
(`melhorar_min:` = melhora mínima para contar).

### Busca em grade

```tilt run
modelo Xor:
  camadas:
    - densa: 2
    - softmax

busca Otima:
  modelo: Xor
  dados: { x: [[0, 0], [0, 1], [1, 0], [1, 1]], y: [0, 0, 0, 0] }
  perda: entropia_cruzada
  epocas: 20
  grade:
    taxa: [0.1, 0.01]
    otimizador: [sgd, adam]
  criterio: perda                    # ou acuracia (só com entropia_cruzada)
```

Roda todas as combinações (máx. 64) com os mesmos dados, imprime a tabela
e deixa os melhores pesos no `modelo`. Aceita os mesmos campos do `treino`
(`lote:`, `semente:`, `validacao:`, ...) menos `checkpoint:`/`retomar:`.

### Dataloader streaming

`carregador "dados.csv" ou "dados.parquet", alvo: "y", fluxo: verdadeiro` não materializa
nada: o `treino` varre o arquivo uma vez e relê CSV em blocos de `bloco:` linhas
ou Parquet por row groups — só o bloco corrente vive na RAM. Equivalente bit a bit
ao treino em RAM (mesma semente, mesmos lotes) e à retomada. Limite: modelo 2D
(denso); CNN continua exigindo dados em RAM.

## GPU

`dispositivo: auto|gpu|"cuda:N"` + `TILT_GPU`:

| `TILT_GPU` | Comportamento |
|---|---|
| `off` (padrão) | CPU sempre |
| `auto` | tenta `dlopen` de `libcuda` + `libnvrtc`, compila os kernels; sem driver → CPU (silencioso) |
| `fake` | roteia `matmul`/`relu` pelo caminho de dispatch da GPU usando math de CPU — saída idêntica, útil para testar sem hardware |

Quando um backend de GPU ativa, imprime uma vez `[gpu] <info>`. O caminho CUDA
foi validado apenas em hardware.

## stdlib: `nn` e `io`

Além de `modelo`/`treino` (que têm otimizadores e backward), a stdlib
instalada com o tilt (`<prefixo>/share/tilt/stdlib`, resolvida automaticamente
por `importar`) traz camadas compostas em tilt puro — legíveis, testáveis e
copiáveis para o projeto:

```tilt run
importar nn

funcao principal:
  q = tensor [[1.0, 0.0], [0.0, 1.0]]
  imprimir nn.atencao_causal(q, q, q, 1.0).dados
```

| Função | Efeito |
|---|---|
| `nn.linear(x, w, b)` | `x @ w + b` (viés na última dimensão) |
| `nn.atencao(q, k, v, escala)` | `softmax((q @ kᵀ) * escala) @ v` |
| `nn.atencao_causal(q, k, v, escala)` | idem, com máscara triangular |
| `nn.feedforward(x, w1, b1, w2, b2)` | MLP com `gelu` |
| `nn.bloco_atencao(x, w_q, w_k, w_v, w_o)` | bloco pré-norm com residual |
| `nn.norma_camada(x)` | layer norm na última dimensão |

`escala` é `1 / raiz(d_k)` — a linguagem ainda não tem `sqrt`, então quem
chama passa a escala (use `1.0` para ignorar). É forward-only: para treinar,
use `modelo`/`treino`. O módulo `io` (guia 03) cobre caminhos, existência e
JSON seguro; `rede` traz `get_json`/`post_json` reais sobre o HTTP genérico
(ver guia 03).
