# Retomada do desenvolvimento

Registro atualizado em 2026-10-01 para continuar na próxima sessão.

## Estado atual

O caminho colunar já funde `scan -> filtro -> derivar numérico -> selecionar -> agrupar` quando as operações são compatíveis. A derivação numérica virtual evita materialização intermediária; expressões complexas, condicionais e alguns caminhos nested continuam usando o fallback geral. O fallback em CPU permanece obrigatório.

Na matriz de 2 milhões de linhas, o Tilt RPC mediu 169/175/386 ms para
agrupamento/filtro/junção; a rodada com códigos dictionary e seleção tardia
marcou 134/161/353 ms. O Tilt já supera pandas, mas continua atrás de Polars e
DuckDB, principalmente em junções. O spill numérico reduziu RSS em 75,6% e
tempo em 33,1% no caso de 500 mil grupos. Os números completos estão em
`benchmarks/data-backends-2026-09-30-2m.md`,
`benchmarks/data-backends-2026-09-30-2m-group-codes.md` e
`benchmarks/aggregate-spill-2026-09-30.md`.

## Próximos passos, nesta ordem

1. **Fechar o planejador lazy**
   - Pushdown local, SQL, Elasticsearch/OpenSearch e Spark via Livy já está
     implementado.
   - Completar nested pushdown, joins/agregações no plano e conectores remotos
     restantes.
   - Ampliar pruning por estatísticas e page index.
   - Manter o mesmo resultado e fallback quando o pushdown não for suportado.

2. **Reduzir a diferença em joins**
   - Paralelizar o hash join completo e adicionar caminho streaming.
   - Repetir joins internos, esquerdos, direitos e compostos com mais amostras.
   - Remover cópias restantes na ponte DuckDB e avaliar Arrow/zero-copy.

3. **Validar e documentar**
   - Repetir benchmarks frios e quentes contra pandas, Polars e DuckDB em 1M,
     10M e 100M linhas.
   - Medir leitura, filtro, projeção, derivação, agregação, materialização, RSS
     e tempo total separadamente.
   - Usar pelo menos 10 repetições nos limites de regressão.

4. **Agregações e leitura**
   - Spill para distintos/quantis e sort externo geral.
   - Pool de leitura CSV/Parquet, page index e projeção nested completa.
   - Reduzir o fallback por linhas em expressões complexas.

5. **GPU CUDA/Metal**
   - Residência persistente geral, batching automático e execução por grafo.
   - Kernels de normalização, pooling e redução.
   - Preservar o fallback CPU; AMD/ROCm continua fora do escopo.

## Fora do escopo atual

AMD/ROCm não será implementado nesta etapa. CUDA/Metal devem continuar funcionando com fallback completo em CPU. A retenção de artefatos de build é uma decisão operacional e não uma pendência de código.
