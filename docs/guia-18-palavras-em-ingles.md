# 18 — Palavras reservadas em inglês

A Tilt é bilíngue: toda palavra-chave, função embutida e método existe em português
(a forma canônica) e em inglês. Escolha o idioma **por arquivo** — o resto da
linguagem (mensagens de erro, documentação, mensagens do compilador) continua em
português.

```tilt run
let rate = 0.001

function center t:
  average = t.mean
  return t - average

pipeline demo:
  steps:
    - table = [{ region: "south", value: 30 }, { region: "north", value: 120 }]
    - summary = table.group_by "region", { total: sum "value", orders: count }
    - print summary
    - for each row in table:
        print row.region
    - if rate >= 0.9:
        print "high"
    else:
        print "low"
    - try:
        x = read_csv "does_not_exist.csv"
    catch err:
      print "failed: {{err}}"
```

## Como o idioma é escolhido

Depois de ler o arquivo, o lexer conta as palavras estruturais que abrem linhas
(`steps:`, `model X:`, `if`, `for each`, `let`... contra `passos:`, `modelo X:`, `se`,
`para cada`, `seja`...). Se as inglesas predominam, o arquivo é lido como inglês; senão
ele fica **exatamente como está** — um programa em português nunca é reinterpretado, mesmo
que use nomes em inglês (`rows`, `count`).

Para forçar, coloque um comentário nas primeiras linhas:

```tilt skip
# language: en      (ou  # idioma: pt  para desligar a tradução)
```

Com o modo inglês ligado, palavras em português continuam valendo (dá para misturar:
`passos:` com `print`). Toda a saída do compilador (`tilt ast`, mensagens `T0xx`) usa a
forma portuguesa.

## O que **não** é traduzido

A tradução é conservadora para não mexer nos seus dados:

- **nomes que você define** (variável, função, parâmetro, variável de `for each`, `catch`,
  parâmetros de `tool`, nomes de declarações) — valem dentro da declaração onde aparecem
  (`function total_of`: um `count` local não impede o `count` de `group_by` em outro
  pipeline);
- **chaves de `{ mapa }`** e campos de blocos `type`, `input`, `output`, `data` (nomes seus:
  `{ type: "a", count: 3 }` continuam assim nas colunas, no JSON e no CSV);
- **membros** (`row.count`, `config.type`): depois de `.` só se traduz método chamado
  com argumentos (`table.filter row.x > 1`) e os métodos sem argumentos da tabela abaixo;
- **elementos soltos de lista** (`features: [count, plan]` — costumam ser colunas);
- **textos** (`"..."`), comentários e o nome de qualquer coluna.

Os campos que o runtime devolve em português (`r.texto`, `r.rastro`, `h.score`) também
respondem pelo nome inglês (`r.text`, `r.trace`) quando o seu mapa não tem uma chave igual.
Valores enumerados que são **texto** (`storage: "memory"`, `provider: "anthropic"`) não
mudam: são strings.

## Vocabulário

### Controle de fluxo, literais e operadores

Valem em qualquer posição (menos como chave de `{ mapa }` ou depois de `.`).

| inglês | português |
|---|---|
| `and` | `e` |
| `as` | `como` |
| `break` | `parar` |
| `catch` | `capturar` |
| `const` | `constante` |
| `contains` | `contem` |
| `continue` | `continuar` |
| `device` | `dispositivo` |
| `each` | `cada` |
| `else` | `senao` |
| `false` | `falso` |
| `for` | `para` |
| `from` | `de` |
| `function` | `funcao` |
| `if` | `se` |
| `import` | `importar` |
| `in` | `em` |
| `let` | `seja` |
| `not` | `nao` |
| `null` | `nulo` |
| `or` | `ou` |
| `return` | `retornar` |
| `true` | `verdadeiro` |
| `try` | `tentar` |
| `while` | `enquanto` |

### Declarações e chaves de bloco

Só no início da linha, antes de `:` (`steps:`, `model Nome:`, `- dense: 512`).

| inglês | português |
|---|---|
| `activation` | `ativacao` |
| `agent` | `agente` |
| `agents` | `agentes` |
| `attempts` | `tentativas` |
| `batch` | `lote` |
| `criterion` | `criterio` |
| `cross_validation` | `validacao_cruzada` |
| `data` | `dados` |
| `dense` | `densa` |
| `depends_on` | `depende_de` |
| `description` | `descricao` |
| `device` | `dispositivo` |
| `dimension` | `dimensao` |
| `dropout` | `abandono` |
| `early_stop` | `parar_cedo` |
| `epochs` | `epocas` |
| `evaluation` | `avaliacao` |
| `every` | `a_cada` |
| `execute` | `executar` |
| `experiment` | `experimento` |
| `fallback` | `reserva` |
| `features` | `atributos` |
| `flow` | `fluxo` |
| `format` | `formato` |
| `goal` | `objetivo` |
| `grid` | `grade` |
| `impute` | `imputar` |
| `index` | `indice` |
| `input` | `entrada` |
| `key` | `chave` |
| `layers` | `camadas` |
| `learning_rate` | `taxa_aprendizado` |
| `loss` | `perda` |
| `max_steps` | `max_passos` |
| `memory` | `memoria` |
| `metric` | `metrica` |
| `metrics` | `metricas` |
| `middleware` | `meio` |
| `model` | `modelo` |
| `not_null` | `nao_nulo` |
| `on_epoch` | `ao_epoca` |
| `on_failure` | `ao_falhar` |
| `on_violation` | `ao_violar` |
| `one_hot` | `um_de_n` |
| `optimizer` | `otimizador` |
| `output` | `saida` |
| `partition_by` | `particionar_por` |
| `path` | `caminho` |
| `port` | `porta` |
| `preprocess` | `pre_processar` |
| `pretrained` | `pesos` |
| `provider` | `provedor` |
| `quarantine` | `quarentena` |
| `query` | `consulta` |
| `range` | `intervalo` |
| `rate` | `taxa` |
| `rate_limit` | `limite_taxa` |
| `respond` | `responder` |
| `respond_stream` | `responder_em_fluxo` |
| `resume` | `retomar` |
| `role` | `papel` |
| `route` | `rota` |
| `sample` | `amostra` |
| `schedule` | `agenda` |
| `scheduler` | `agendador` |
| `search` | `busca` |
| `seed` | `semente` |
| `service` | `servico` |
| `shuffle` | `embaralhar` |
| `source` | `fonte` |
| `split` | `dividir` |
| `standardize` | `padronizar` |
| `steps` | `passos` |
| `storage` | `armazenamento` |
| `strategy` | `estrategia` |
| `system` | `sistema` |
| `target` | `alvo` |
| `task` | `tarefa` |
| `team` | `equipe` |
| `temperature` | `temperatura` |
| `test` | `teste` |
| `threshold` | `limiar` |
| `time_limit` | `tempo_limite` |
| `token_cap` | `teto_tokens` |
| `tolerance` | `tolerancia` |
| `tool` | `ferramenta` |
| `tools` | `ferramentas` |
| `topic` | `topico` |
| `train` | `treino` |
| `type` | `tipo` |
| `unique` | `unico` |
| `user` | `usuario` |
| `validation` | `validacao` |
| `verbose` | `verboso` |
| `verify` | `verificar` |
| `weights` | `pesos` |
| `window` | `janela` |

### Funções embutidas e da biblioteca padrão

Em qualquer expressão, exceto se o arquivo define um nome igual.

| inglês | português |
|---|---|
| `abort` | `abortar` |
| `all` | `todos` |
| `any` | `qualquer` |
| `append_delta` | `anexar_delta` |
| `append_iceberg` | `anexar_iceberg` |
| `append_text` | `anexar_texto` |
| `ask` | `perguntar` |
| `ask_stream` | `perguntar_em_fluxo` |
| `assert` | `afirmar` |
| `assert_equal` | `afirmar_igual` |
| `base64_decode` | `base64_decodificar` |
| `base64_encode` | `base64_codificar` |
| `boolean` | `logico` |
| `call_python` | `chamar_python` |
| `ceil` | `teto` |
| `copy_s3` | `copiar_s3` |
| `cos` | `cosseno` |
| `count` | `contar` |
| `delete_s3` | `apagar_s3` |
| `embed` | `incorporar` |
| `ends_with` | `termina_com` |
| `enumerate` | `enumerar` |
| `epoch` | `epoca` |
| `experiment` | `experimento` |
| `filter` | `filtrar` |
| `float` | `decimal` |
| `floor` | `piso` |
| `format_date` | `formatar_data` |
| `input` | `entrada` |
| `integer` | `inteiro` |
| `join` | `juntar` |
| `json_read` | `json_ler` |
| `json_text` | `json_texto` |
| `keys` | `chaves` |
| `length` | `tamanho` |
| `list` | `lista` |
| `list_files` | `listar_arquivos` |
| `list_s3` | `listar_s3` |
| `ln` | `logaritmo` |
| `loader` | `carregador` |
| `lower` | `minusculas` |
| `map` | `mapear` |
| `mean` | `media` |
| `metrics` | `metricas` |
| `model` | `modelo` |
| `now` | `agora` |
| `ones` | `uns` |
| `optimize_delta` | `otimizar_delta` |
| `optimize_iceberg` | `otimizar_iceberg` |
| `optional` | `opcional` |
| `power` | `potencia` |
| `print` | `imprimir` |
| `probability` | `probabilidade` |
| `query_sql` | `consultar_sql` |
| `random` | `aleatorio` |
| `range` | `intervalo` |
| `read` | `ler` |
| `read_csv` | `ler_csv` |
| `read_delta` | `ler_delta` |
| `read_iceberg` | `ler_iceberg` |
| `read_json` | `ler_json` |
| `read_kafka` | `ler_kafka` |
| `read_parquet` | `ler_parquet` |
| `read_redis` | `ler_redis` |
| `read_s3` | `ler_s3` |
| `read_text` | `ler_texto` |
| `reduce` | `reduzir` |
| `regex_extract` | `regex_extrair` |
| `regex_match` | `regex_casa` |
| `regex_replace` | `regex_substituir` |
| `register` | `registrar` |
| `remove_file` | `remover_arquivo` |
| `replace` | `substituir` |
| `request_logging` | `registro_requisicoes` |
| `rerank` | `reranquear` |
| `respond` | `responder` |
| `respond_stream` | `responder_em_fluxo` |
| `result` | `resultado` |
| `retry` | `repetir` |
| `reverse` | `reverso` |
| `round` | `arredondar` |
| `row` | `linha` |
| `rows` | `linhas` |
| `run_sql` | `executar_sql` |
| `sin` | `seno` |
| `size` | `tamanho` |
| `sleep` | `dormir` |
| `sort` | `ordenar` |
| `split` | `dividir` |
| `split_text` | `dividir_texto` |
| `sqrt` | `raiz` |
| `starts_with` | `comeca_com` |
| `step` | `passo` |
| `stream` | `fluxo` |
| `sum` | `somar` |
| `table` | `tabela` |
| `tan` | `tangente` |
| `text` | `texto` |
| `trace` | `rastro` |
| `transaction` | `transacao` |
| `trim` | `aparar` |
| `type_of` | `tipo_de` |
| `unique` | `unicos` |
| `upper` | `maiusculas` |
| `values` | `valores` |
| `warn` | `avisar` |
| `write_csv` | `escrever_csv` |
| `write_delta` | `escrever_delta` |
| `write_iceberg` | `escrever_iceberg` |
| `write_json` | `escrever_json` |
| `write_kafka` | `escrever_kafka` |
| `write_parquet` | `escrever_parquet` |
| `write_redis` | `escrever_redis` |
| `write_s3` | `escrever_s3` |
| `write_text` | `escrever_texto` |

### Valores do vocabulário

`loss: cross_entropy`, `metrics: [accuracy]`, `memory: conversation`...

| inglês | português |
|---|---|
| `accuracy` | `acuracia` |
| `conversation` | `conversa` |
| `cosine` | `cosseno` |
| `cross_entropy` | `entropia_cruzada` |
| `gradient_boosting` | `gradiente_impulsionado` |
| `impute` | `imputar` |
| `linear_regression` | `regressao_linear` |
| `logistic_regression` | `regressao_logistica` |
| `one_hot` | `um_de_n` |
| `parallel` | `paralelo` |
| `quadratic` | `quadratica` |
| `random_forest` | `floresta_aleatoria` |
| `sequential` | `sequencial` |
| `standardize` | `padronizar` |
| `vector` | `vetorial` |

### Métodos (depois de `.`, com argumentos)

`tabela.group_by "x", {...}`, `modelo.run entrada`.

| inglês | português |
|---|---|
| `ask` | `perguntar` |
| `derive` | `derivar` |
| `export_gguf` | `exportar_gguf` |
| `export_onnx` | `exportar_onnx` |
| `filter` | `filtrar` |
| `forward` | `para_frente` |
| `group_by` | `agrupar_por` |
| `head` | `primeiros` |
| `insert` | `inserir` |
| `limit` | `limite` |
| `load_weights` | `carregar_pesos` |
| `map` | `mapear` |
| `order_by` | `ordenar_por` |
| `predict` | `prever` |
| `reshape` | `reformar` |
| `respond` | `responder` |
| `run` | `executar` |
| `save_weights` | `salvar_pesos` |
| `search` | `buscar` |
| `select` | `selecionar` |
| `sum` | `soma` |

### Métodos sem argumentos (depois de `.`)

`t.shape`, `t.transpose` — não vale para `row.`/`input.`/`result.`/`step.`.

| inglês | português |
|---|---|
| `distinct` | `distinto` |
| `lower` | `minusculas` |
| `mean` | `media` |
| `shape` | `forma` |
| `transpose` | `transposta` |
| `upper` | `maiusculas` |

### Argumentos nomeados

`ask gpt, user: "..."` fora de `{ }`.

| inglês | português |
|---|---|
| `format` | `formato` |
| `partition_by` | `particionar_por` |
| `system` | `sistema` |
| `user` | `usuario` |
| `versions` | `versoes` |
| `wait` | `espera` |

### Chaves fixas de mapa

`split: { train: 0.8, test: 0.2 }`.

| inglês | português |
|---|---|
| `per_minute` | `por_minuto` |
| `test` | `teste` |
| `train` | `treino` |
| `validation` | `validacao` |

### Nomes de tipo

`text`, `list[text]`, `-> integer` — valem mesmo se o arquivo tem uma variável `text`.

| inglês | português |
|---|---|
| `boolean` | `logico` |
| `float` | `decimal` |
| `integer` | `inteiro` |
| `list` | `lista` |
| `optional` | `opcional` |
| `stream` | `fluxo` |
| `table` | `tabela` |
| `text` | `texto` |

Para acrescentar uma palavra, edite a tabela em `src/lexer/aliases_en.cpp` (uma linha
`{"inglês", "português", "papéis"}`) e regenere este capítulo; `tests/aliases/*.en.tilt`
guardam programas em inglês equivalentes aos exemplos da documentação e o teste
`aliases_en` confere que o lexer os reduz às mesmas palavras.
