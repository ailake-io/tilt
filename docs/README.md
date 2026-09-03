# Guia da linguagem Tilt

Documentação de uso. Para a especificação arquitetural do compilador veja
[`../CLAUDE.md`](../CLAUDE.md); para instalação e visão geral veja
[`../README.md`](../README.md).

| # | Capítulo | Conteúdo |
|---|---|---|
| 01 | [Sintaxe](guia-01-sintaxe.md) | indentação, comentários, literais, `seja`/`constante`, `funcao`, `se`/`para cada`/`enquanto`/`tentar`, operadores, coleções, `importar` |
| 02 | [Tipos](guia-02-tipos.md) | `tipo`, escalares, `lista`/`mapa`/`opcional`, `tensor`, `tabela`, `fluxo`, união de literais, valores padrão |
| 03 | [Engenharia de dados](guia-03-dados.md) | `fonte`, `pipeline`, `passos`, métodos de tabela, `verificar`, `ao_falhar`, `agenda`, CSV/JSON |
| 04 | [ML e Deep Learning](guia-04-ml-dl.md) | tensores e ops, `modelo`/`camadas`, `modelo X.executar`, `treino`, `carregador`, otimizadores, `dispositivo`/GPU |
| 05 | [LLMs e RAG](guia-05-llm-rag.md) | `llm`, `perguntar`, `formato: <tipo>`, `incorporar`, `dividir_texto`, `indice`, `.inserir`/`.buscar` |
| 06 | [Agentes](guia-06-agentes.md) | `ferramenta`, `agente`, `.responder`, `rastro`, `memoria`, `equipe`, `estrategia` |
| 07 | [Serviços HTTP](guia-07-http.md) | `servico`, `rota`, `entrada`, `responder`, `tilt servir`, validação, códigos de status |
| 08 | [CLI](guia-08-cli.md) | cada comando, flags, códigos de saída, variáveis de ambiente |
| 09 | [VM e nativo](guia-09-vm-nativo.md) | subconjunto da VM de bytecode e do codegen x86-64, limitações |
| 10 | [IA e editores](guia-10-ia-editores.md) | `checar --json`, `referencia`, `checar_tilt`, `tilt lsp`/`completar`, Neovim, VS Code |
| 11 | [Diagnósticos](guia-11-diagnosticos.md) | todos os códigos `Tnnn` com exemplo |
| 12 | [Limitações](guia-12-limitacoes.md) | status da 1ª passada — o que ainda não funciona |
