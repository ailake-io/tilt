# Roadmap de performance: aproximar Polars e DuckDB

Este plano foi elaborado a partir do benchmark de 2 milhões de linhas em
`data-backends-2026-09-30-2m.md` e da comparação com as arquiteturas descritas
na documentação oficial do Polars, DuckDB e Apache Arrow.

## Referência atual

No processo RPC, a rodada de referência em 2 milhões de linhas mediu 169,026 ms
para agrupamento, 174,722 ms para filtro e 385,664 ms para junção. A mesma
matriz marcou 62,583/52,801/58,563 ms no Polars e
112,663/101,145/96,519 ms no DuckDB. O Tilt já supera pandas, mas a junção
continua sendo o maior desvio em relação aos motores colunares.

As otimizações seguintes registraram 134,493 ms no agrupamento por códigos
dictionary, 160,630 ms no filtro e 344,111 ms na junção. A junção teve uma
repetição posterior de 366,825 ms; esses números são progresso observado, ainda
sem amostras suficientes para estabelecer um limite de regressão.

## Ordem de implementação

1. **Join colunar tipado — entregue em parte**
   - [x] Usar códigos compactos para textos dictionary encoded.
   - [x] Eliminar a serialização do valor textual em cada linha.
   - [x] Manter igualdade exata e semântica de nulos.
   - [x] Cache reutilizável, merge join ordenado e chaves compostas tipadas.
   - [ ] Paralelizar o hash join completo, suportar streaming e repetir a matriz
     de joins internos, esquerdos, direitos e compostos com mais amostras.

2. **Executor por lotes — entregue**
   - [x] Lotes de 2.048–8.192 linhas com buffers tipados e bitmap de nulos.
   - [x] Fusão `scan -> filter -> project -> aggregate` para o subconjunto
     vetorizável.
   - [x] Seleção tardia, projeção sem cópia e pool global de workers.
   - [x] Códigos dictionary usados diretamente em agrupamentos e distintos.

3. **Expressões e filtros — parcial**
   - [x] `derivar`/`mapear` aritméticos e colunas virtuais no plano filtrado.
   - [x] Filtros compostos e seleção tardia no mesmo passe da agregação.
   - [ ] IR tipado completo, constant folding e eliminação de subexpressões.
   - [ ] Remover o fallback por linhas para expressões complexas e nested.

4. **Leitura CSV e Parquet — parcial**
   - [x] CSV simples escreve diretamente em colunas tipadas.
   - [x] Projeção, codecs gzip/Snappy/Zstd/Brotli/LZ4 e pruning básico Parquet.
   - [x] Projeção nested e limite sem predicado evitam decodificação desnecessária.
   - [ ] Pool de leitura, page index completo e projeção nested sem fallback em
     todos os níveis.

5. **Planejador lazy — parcial**
   - [x] Plano de scan, filtro, projeção e limite em CSV, Parquet e Delta.
   - [x] Pushdown nativo para SQL, Elasticsearch/OpenSearch e Spark via Livy.
   - [x] Fallback residual correto para conectores sem tradução.
   - [ ] Nested pushdown, joins/agregações no plano lógico e conectores remotos
     restantes.

6. **Agregações, ordenação e memória — parcial**
   - [x] Acumuladores por thread, SIMD, radix sort, quantis aproximados e spill
     numérico automático/seletivo.
   - [x] Métricas de memória, pico por row group e cache de joins.
   - [ ] Spill para distintos/quantis, sort externo geral e arena por lote.

7. **Delegação e validação — parcial**
   - [x] Enviar blocos colunares ao DuckDB sem materializar linha a linha;
     junções também preservam o schema colunar.
   - [ ] Arrow/zero-copy, conexão/prepared statements persistentes e
     materialização compartilhada.
   - [x] Matriz inicial contra pandas, Polars e DuckDB em 1M/2M linhas.
   - [ ] Separar leitura, execução e materialização em cache frio/quente e
     repetir em 10M/100M linhas.

8. **GPU CUDA/Metal — parcial**
   - [x] Kernels principais e fallback CPU validados.
   - [ ] Residência persistente geral, batching automático, execução por grafo,
     normalização, pooling e redução.

## Metas de aceitação

- Agrupamento e filtro nativos em até 1,25x o DuckDB no benchmark de referência.
- Join nativo em até 1,5x o DuckDB.
- Nenhuma regressão no fallback de CPU ou na semântica de nulos.
- Medição separada de leitura, execução, materialização e escrita.
- Execução maior que a memória disponível com spill configurável.

## Fontes técnicas

- Polars: [otimizações lazy](https://docs.pola.rs/user-guide/lazy/optimizations/)
  e [execução streaming](https://docs.pola.rs/user-guide/concepts/streaming/).
- DuckDB: [execução vetorizada](https://duckdb.org/docs/lts/internals/vector),
  [otimizações internas](https://duckdb.org/docs/current/internals/overview),
  [Parquet](https://duckdb.org/docs/current/data/parquet/overview) e
  [ajuste de cargas](https://duckdb.org/docs/current/guides/performance/how_to_tune_workloads).
- Arrow: [Acero](https://arrow.apache.org/docs/dev/cpp/acero/overview.html) e
  [gerenciamento de threads](https://arrow.apache.org/docs/cpp/threading.html).
