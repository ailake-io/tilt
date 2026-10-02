# Checkpoint de performance — atualização 01/10/2026

Estado salvo para a próxima sessão.

## Entregue

- Seleção tardia para filtros colunares, com selection vector e máscaras
  compostas em lotes.
- Projeção colunar sem cópia sobre visões filtradas.
- `derivar` e `mapear` aritméticos consumindo apenas as linhas selecionadas.
- Pool global reutilizável, limitado a 16 workers, para agregações e índices de
  join.
- `agrupar_por` resolve o parent e os índices físicos uma vez por operação.
- `distintos` usa códigos `uint32` em textos dictionary encoded.
- Fusão `scan -> filtro -> seleção -> derivação -> agrupação` no subconjunto
  vetorizável, com seleção tardia e projeção sem cópia.
- Spill numérico seletivo/automático e métricas de memória, row group e joins.
- Pushdown lazy em CSV, Parquet, Delta, SQL, Elasticsearch/OpenSearch e Spark
  via Livy; o Spark real foi validado em container na porta 8998.
- Ponte DuckDB por blocos colunares e junções sem materialização só para
  descobrir o schema.

## Validação

- Build Release concluído.
- CTest colunar/golden/Parquet: 4/4.
- Pipeline filtrado, projetado, derivado e agrupado validado em 200 mil linhas.
- `git diff --check` sem erros.
- Matriz focada atual: 8/8 testes (incluindo Livy).
- Em 2 milhões de linhas: agrupamento/filtro/junção RPC de referência
  169/175/386 ms; rodada otimizada 134/161/353 ms.
- Spill em 500 mil grupos: −33,1% de tempo e −75,6% de RSS.

## Próximo trabalho

O spill externo numérico já foi implementado atrás de `TILT_AGG_SPILL=1`:
particiona por hash e reduz uma partição por vez. Os acumuladores por grupo já
foram compactados: estados de conjuntos e amostras são criados apenas quando o
plano contém essas funções. A primeira medição foi registrada em
`aggregate-spill-2026-09-30.md`: em 2 milhões de linhas/500 mil grupos, o spill
reduziu RSS em 75,6% e tempo em 33,1%. Faltam outras cardinalidades e a
comparação com pandas, Polars e DuckDB.

A matriz já foi concluída em `cardinality-2026-09-30.md`. O spill deve
permanecer seletivo: ele só vence em cardinalidade alta. A seleção automática
por cardinalidade foi entregue em `TILT_AGG_SPILL=auto`; adicionar spill para
`distintos` e quantis continua pendente.

O maior desvio permanece no join. A próxima rodada deve medir joins paralelos e
streaming com pelo menos 10 repetições, separando leitura, execução e
materialização, além de testar cache frio/quente em 1M, 10M e 100M linhas.
Arrow/zero-copy, nested pushdown completo, sort externo geral e residência
persistente CUDA/Metal continuam como ganhos posteriores.
