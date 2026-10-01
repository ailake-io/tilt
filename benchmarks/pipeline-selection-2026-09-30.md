# Pipeline colunar com seleção tardia

O teste gerou 1 milhão de linhas com as colunas `id`, `regiao`, `valor` e
`extra`, executou em uma sessão RPC persistente e repetiu cada pipeline quatro
vezes. A medição inclui a leitura CSV, filtro e agrupamento.

| pipeline | mediana ms |
| --- | ---: |
| filtro → agrupar | 97,637 |
| filtro → selecionar(`regiao`, `valor`) → agrupar | 90,687 |

A projeção de duas colunas ficou aproximadamente 7,1% mais rápida na rodada,
porque descarta `id` e `extra` antes da agregação e evita criar estruturas para
essas colunas. É um resultado preliminar; novas rodadas devem separar leitura,
filtro e agregação para medir o ganho isolado.
