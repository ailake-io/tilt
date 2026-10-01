# Pipeline colunar fundido — 30/09/2026

Dataset CSV sintético com 200.000 linhas (`id`, `valor`, `regiao`), executado
no binário Release local em três repetições por pipeline. A medição inclui
leitura, filtro e agregação.

| pipeline | mediana (ms) |
| --- | ---: |
| filtro → agrupar | 27,565 |
| filtro → selecionar → agrupar | 28,180 |

`selecionar` permanece praticamente neutro porque virou uma visão sem cópia;
os dois caminhos agregam diretamente o parent e o vetor de seleção. Os valores
são uma referência local, não um baseline entre máquinas.
