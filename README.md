# Tilt

Linguagem declarativa estilo YAML para **Machine Learning, Deep Learning,
Engenharia de Dados, LLMs e Agentes de IA**. Compilador e runtime em C++20,
sem LLVM e sem Rust.

Um mesmo arquivo `.tilt` descreve um pipeline de dados, treina um modelo, chama
um LLM, orquestra agentes e sobe um serviço HTTP — com a mesma sintaxe: 2
espaços de indentação, `chave: valor`, listas com `-`, sem `{ }`, `;` nem `()`
ruidosos.

```tilt
pipeline resumo_vendas:
  passos:
    - vendas = ler_csv "vendas.csv"
    - grandes = vendas.filtrar linha.valor >= 50
    - por_regiao = grandes.agrupar_por "regiao", { receita: somar "valor", n: contar }
    - para cada r em por_regiao:
        imprimir r.regiao, "->", r.receita, "(", r.n, "pedidos )"
```

- **Especificação da linguagem:** [`CLAUDE.md`](CLAUDE.md)
- **Guia de uso (13 capítulos):** [`docs/`](docs/README.md)
- **Repositório:** https://github.com/ailake-io/tilt

---

## Estado — `v0.1.0`

Toolchain interpretada **completa** (1ª passada de cada marco). O que roda hoje:

| Área | Funciona | Ainda é stub |
|---|---|---|
| Sintaxe | lexer, parser, semântica (`checar`), interpretador de árvore | literais `[...]`/`{...}` multilinha |
| Dados | CSV e JSON (leitura/escrita), `fonte` de arquivo local, `pipeline`, `verificar`, `ao_falhar`, `agenda` (validação) | Postgres/Kafka/S3/Parquet, streaming `janela`, loop de agenda |
| ML/DL | tensores f32 CPU, `modelo` (inferência), `treino` (backprop + SGD/Adam), `carregador` | GPU só validado em `fake`; carga de `pesos:` de arquivo |
| LLM/RAG | `perguntar`, saída estruturada por `tipo`, `incorporar`, `indice` em memória (cosseno) | `qdrant`/`pgvector`; rede real precisa de `curl` |
| Agentes | `ferramenta`, `agente.responder`, `equipe` (sequencial/paralelo) | planner iterativo, `estrategia: supervisor` |
| HTTP | `servico`/`rota`, validação de `entrada:`, `tilt servir` | epoll + keep-alive, `meio:` (middleware) |
| Execução | VM de bytecode p/ `funcao` pura, codegen nativo x86-64 (subconjunto inteiro) | VM/codegen para o programa inteiro |
| Tooling | `checar --json`, `referencia`, `tilt lsp` + `completar`, `checar_tilt` | extensão de editor publicada |

Detalhes em [`docs/guia-12-limitacoes.md`](docs/guia-12-limitacoes.md).

---

## Instalação

### Do código-fonte

```bash
git clone https://github.com/ailake-io/tilt
cd tilt
sh scripts/install.sh                 # instala em ~/.local
# ou:  sh scripts/install.sh --prefix=/usr/local     (talvez com sudo)
```

Requisitos: `cmake >= 3.20`, compilador C++20 (GCC 11+ / Clang 14+). Instala
`tilt` em `<prefix>/bin` e os exemplos em `<prefix>/share/tilt`. Remover:
`sh scripts/install.sh --uninstall`.

### Binários pré-compilados

Cada tag `v*` publica `tilt-<versao>-<os>-<arch>.tar.gz` (+ `.sha256`) em
[Releases](https://github.com/ailake-io/tilt/releases): Linux x86_64 (estático,
sem dependência de libstdc++) e macOS arm64. Extraia e ponha `bin/tilt` no
`PATH`. `curl` é necessário em runtime apenas para chamadas reais de LLM.

### Pacote local

```bash
cmake --preset release && cmake --build --preset release
cd build/release && cpack           # gera tilt-<versao>-<os>-<arch>.tar.gz
```

---

## Primeiro programa

```tilt
# ola.tilt
funcao faixa n -> texto:
  se n >= 100:
    retornar "grande"
  retornar "pequena"

pipeline ola:
  passos:
    - total = 40 + 62
    - imprimir "total:", total, "->", faixa(total)
    - para cada k em intervalo 3:
        imprimir "  item", k
```

```bash
tilt checar ola.tilt      # valida sintaxe, indentação e tipos
tilt executar ola.tilt    # roda
```

```
== pipeline ola ==
total: 102 -> grande
  item 0
  item 1
  item 2
```

---

## Comandos

| Comando | Descrição |
|---|---|
| `tilt checar <a> [--json]` | valida sintaxe, indentação, tipos, segredos, dispositivos, referências |
| `tilt executar <a> [--agendar]` | roda no interpretador (VM para `funcao` pura) |
| `tilt servir <a> [--porta N] [--requisicoes N]` | sobe o `servico` HTTP declarado |
| `tilt compilar <a> --saida <bin> [--asm]` | binário nativo x86-64 (subconjunto inteiro) |
| `tilt completar <a> --linha L --coluna C [--json]` | candidatos de autocomplete |
| `tilt lsp` | servidor Language Server (stdio) |
| `tilt referencia` | referência compacta da linguagem |
| `tilt ast <a>` / `tilt tokens <a>` | despeja AST / tokens (debug) |
| `tilt versao` / `tilt ajuda` | versão / ajuda |

Detalhe de cada flag e código de saída em [`docs/guia-08-cli.md`](docs/guia-08-cli.md).

### Variáveis de ambiente

| Var | Efeito |
|---|---|
| `TILT_LLM` | `mock` (respostas/embeddings determinísticos, offline) · vazio → `curl` real |
| `TILT_GPU` | `off` (padrão) · `auto` (CUDA se `dispositivo:` pedir) · `fake` (dispatch de GPU com math de CPU) |
| `TILT_VM_DEBUG` | `1` despeja o bytecode das `funcao`s compiladas |
| `CC` | compilador C usado por `tilt compilar` (padrão `cc`) |
| `NO_COLOR` | desliga cor nos diagnósticos |

---

## Exemplos

`exemplos/` (instalados em `<prefix>/share/tilt/exemplos/`):

| Arquivo | Mostra | Como rodar |
|---|---|---|
| `soma.tilt` | cômputo, `funcao`, laço | `tilt executar` |
| `resumo_vendas.tilt` | CSV → filtrar/agrupar/agregar | `tilt executar` |
| `qualidade.tilt` | `verificar` (qualidade de dados), `ordenar_por` | `tilt executar` |
| `fontes.tilt` | `fonte` JSON, `escrever_json` | `tilt executar` |
| `inferencia.tilt` | tensores, `modelo`, forward CPU | `tilt executar` |
| `treino.tilt` | `treino` (backprop + Adam), `carregador` | `tilt executar` |
| `rag_llm.tilt` | `indice` (RAG) + `perguntar` | `TILT_LLM=mock tilt executar` |
| `agente.tilt` | `ferramenta` + `agente.responder` | `TILT_LLM=mock tilt executar` |
| `copiloto.tilt` | agente que revisa Tilt via `checar_tilt` | `TILT_LLM=mock tilt executar` |
| `servico.tilt` | `servico` / `rota` HTTP | `tilt servir` |
| `nativo.tilt` | subconjunto compilável (fatorial, ackermann) | `tilt compilar` |

---

## Documentação

| # | Guia |
|---|---|
| 01 | [Sintaxe](docs/guia-01-sintaxe.md) — indentação, literais, variáveis, `funcao`, controle de fluxo, operadores |
| 02 | [Tipos](docs/guia-02-tipos.md) — `tipo`, escalares, `lista`/`mapa`/`opcional`, `tensor`, `tabela`, `fluxo`, união-literal |
| 03 | [Engenharia de dados](docs/guia-03-dados.md) — `fonte`, `pipeline`, métodos de tabela, `verificar`, `ao_falhar`, `agenda` |
| 04 | [ML e Deep Learning](docs/guia-04-ml-dl.md) — tensores, `modelo`, `treino`, `carregador`, `dispositivo`/GPU |
| 05 | [LLMs e RAG](docs/guia-05-llm-rag.md) — `llm`, `perguntar`, saída estruturada, `incorporar`, `indice` |
| 06 | [Agentes](docs/guia-06-agentes.md) — `ferramenta`, `agente`, `equipe`, `memoria` |
| 07 | [Serviços HTTP](docs/guia-07-http.md) — `servico`, `rota`, `entrada`, `responder`, `tilt servir` |
| 08 | [CLI](docs/guia-08-cli.md) — cada comando, flags, códigos de saída, variáveis de ambiente |
| 09 | [VM e nativo](docs/guia-09-vm-nativo.md) — subconjunto da VM de bytecode e do codegen |
| 10 | [IA e editores](docs/guia-10-ia-editores.md) — `checar --json`, `referencia`, `checar_tilt`, LSP, Neovim |
| 11 | [Diagnósticos](docs/guia-11-diagnosticos.md) — todos os códigos `Tnnn` |
| 12 | [Limitações](docs/guia-12-limitacoes.md) — status da 1ª passada |

---

## Arquitetura

```text
Fonte .tilt
   │
   ▼  src/lexer/       -> tokens (motor de indentação INDENT/DEDENT)
   ▼  src/parser/      -> AST (descida recursiva, std::unique_ptr)
   ▼  src/semantic/    -> símbolos, tipos, formas de tensor  (tilt checar)
   ▼  src/interp/      -> interpretador de árvore            (tilt executar)
   │     └─ src/vm/       funcao pura -> bytecode -> VM de pilha
   │     └─ src/codegen/  subconjunto inteiro -> Assembly x86-64  (tilt compilar)
   └─ src/runtime/     value, tensor, json, llm, vectorstore, http_server, gpu_runtime
      src/lsp/          completion + servidor LSP              (tilt lsp)
```

Layout do repositório em [`CLAUDE.md` §4](CLAUDE.md).

---

## Desenvolvimento

```bash
cmake --preset debug     # AddressSanitizer + UBSan + -Werror
cmake --build --preset debug
ctest --preset debug --output-on-failure     # golden + http + native + lsp
```

Testes de ouro em `tests/golden/` (`entrada.tilt` → `esperado.*`); regenerar:
`UPDATE=1 sh tests/run_golden.sh ./build/release/bin/tilt tests/golden`.
CI: `.github/workflows/ci.yml` (Linux gcc + macOS + sanitizers) e
`release.yml` (tag `v*` → tarballs no GitHub Release).

---

## Roadmap

`M0`–`M12` + instalador + GPU + tooling de IA concluídos (1ª passada). A seguir:
conectores de rede reais, VM/codegen para o programa inteiro (texto, decimal,
tabelas), planner iterativo de agente, epoll no `servico`, extensão de editor.

## Licença

MIT — ver [`LICENSE`](LICENSE).
