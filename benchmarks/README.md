# Tensor operation benchmark

This benchmark compares the C++ Tilt runtime implementations of matmul and
conv2d with NumPy and PyTorch when those packages are available. The C++
microbenchmark is compiled in a temporary directory and does not change the
main build.

From the repository root:

    python3 scripts/benchmark_tensor_ops.py --iterations 20 --warmup 5

For a short run:

    python3 scripts/benchmark_tensor_ops.py --quick --iterations 3 --warmup 1

Use --backend tilt, --backend numpy, or --backend torch to select one backend.
The default all mode skips missing NumPy/PyTorch and reports that on stderr.
The --json option emits records for automation. Timings include the result
reduction used to keep the operation observable; compare runs on the same
machine and with the same backend thread settings.
For CPU-only deployments, compare `TILT_BLAS=off` with an optimized CBLAS
library (for example `TILT_BLAS_LIBRARY=/path/to/libmkl_rt.so`). Tilt loads
OpenBLAS/oneMKL automatically when they are on the library path; without one,
the portable C++ kernel remains available. Set `OPENBLAS_NUM_THREADS` or
`MKL_NUM_THREADS` consistently with the NumPy reference.

For data work, `python3 scripts/benchmark_data_stack.py /path/to/tilt`
measures the same 1-million-row CSV read, groupby, and Parquet write in Tilt
and pandas; it verifies that the aggregate values match before timing. It also
compares projected reads (`selecionar:`), the opt-in typed columnar path
(`colunar: verdadeiro`), and adds Polars/DuckDB when installed.
For the complete data matrix, use `python3 scripts/benchmark_data_backends.py
/path/to/tilt --rows 200000 --repetitions 5`. It measures aggregation, filtered
aggregation and joins in Tilt, pandas, Polars and DuckDB. The latter two are
optional and missing packages are listed as `backends_unavailable` on stderr.
An isolated environment is enough to install them without changing the project:

    python3 -m venv /tmp/tilt-bench-venv
    /tmp/tilt-bench-venv/bin/python -m pip install pandas pyarrow polars duckdb
    /tmp/tilt-bench-venv/bin/python scripts/benchmark_data_backends.py \
        build/release/bin/tilt --rows 1000000 --repetitions 5

The pandas/Polars/DuckDB timings exclude Python import time, while Tilt timings
include process startup. Compare changes within a backend before interpreting
cross-backend ratios.
For language overhead, `python3 scripts/benchmark_language_core.py /path/to/tilt`
compares a 3-million-step integer loop and `fib(30)` with CPython using the
same algorithm and subprocess timing for both runtimes.
For Parquet reads, `python3 scripts/benchmark_parquet_columnar.py /path/to/tilt`
compares row and column modes with pandas on the same 1-million-row file and
checks every aggregate before reporting medians. Pandas import is excluded;
each Tilt sample starts a process. Use `--plain` to disable Parquet dictionary
encoding for a separate comparison.
`python3 scripts/benchmark_parquet_nested.py /path/to/tilt` measures list and
struct reads over multiple row groups, including peak memory on Linux.
`python3 scripts/benchmark_distributed_columnar.py /path/to/tilt` partitions
the same input across 1, 2 and 4 local processes and reports read, read+groupby
and final merge times. It does not include network or shared-filesystem cost.
`python3 scripts/benchmark_cluster_training.py /path/to/tilt` measures Parquet
training with 1, 2 and 4 ranks and reports label-read and checkpoint/barrier
time using optional `TILT_CLUSTER_PROFILE` instrumentation. The shared
directory is local; multi-host latency is not represented.
For the GPU crossover, build the `gpu_dispatch_benchmark` target and run
`TILT_GPU=auto /path/to/gpu_dispatch_benchmark 7`. It measures CPU and CUDA
with host/device copies included and prints CPU-only results when CUDA is
absent. Use `TILT_BLAS_LIBRARY` and a fixed BLAS thread count to compare
against an optimized CPU. See [the measured report](relatorio-2026-09-24.md)
for workload details, results and remaining bottlenecks.
For repeated HTTP calls, `python3 scripts/benchmark_http_client.py /path/to/tilt`
compares the persistent libcurl backend with the CLI fallback on a local
HTTP/1.1 server; this needs loopback socket permission.

Release builds can opt into link-time optimization with `-DTILT_LTO=ON`. GCC
profile-guided builds use `-DTILT_PGO=GENERATE -DTILT_PGO_PROFILE_DIR=/tmp/tilt-pgo`
for training, followed by reconfiguring **the same build directory** with
`-DTILT_PGO=USE` and rebuilding. Train with workloads representative of the
deployment; use a separate build directory from the normal release build.
Both options are off by default and do not require a GPU.

### Sharding de treino

Um bloco treino pode selecionar uma particao deterministica das linhas com
num_shards: N e shard_id: K, usando o mesmo esquema round-robin nos dados em
RAM, no fluxo CSV e no fluxo Parquet. Cada processo/worker recebe K em
0..N-1; com num_shards: 1 o comportamento permanece o mesmo. A divisao de
validacao e feita dentro das linhas do shard.

### Pool de objetos de runtime

`python3 scripts/benchmark_value_pool.py` compara o pool atual com `std::make_shared`
para materialização, join de linhas e criação de listas. Compila variantes temporárias,
valida todos os resultados e emite JSON com amostras, medianas e pico RSS no Linux.
Requer CMake, compilador C++20 e build Release configurado (`--build-dir`).
Veja [método, resultados e limitações](value-pool-2026-09-26.md).

O benchmark do pool agora mede criação e destruição separadas com `--threads 1 2 4`
(100 mil objetos por thread por padrão). A variante sem sincronização roda somente
com uma thread e serve apenas para diagnóstico. As amostras JSON contêm
`create_ms`, `destroy_ms`, `peak_rss_kib` e `checksum`; o tempo do ciclo é a soma
das duas fases, com validação fora do cronômetro. Veja o
[relatório de concorrência](value-pool-lifecycle-2026-09-26.md) e a
[correção da materialização](value-pool-materialization-fix-2026-09-26.md).

### Listas e transferência entre threads

`python3 scripts/benchmark_list_handoff.py` compara somente a alocação de
`Value::lista`, mantendo mapas e tabelas idênticos. Cobre 0, 2, 32 e 256 elementos,
com 1, 2 e 4 threads, liberação local e em outro worker. O JSON registra as
amostras e o pico RSS. Veja [método e resultados](list-handoff-2026-09-26.md).

Após essa investigação, listas usam `std::make_shared`; mapas gerais e o contêiner
de tabelas mantêm o pool. `benchmark_value_pool.py` preserva a comparação
histórica com listas forçadas ao pool nas variantes pool/unsynchronized; essas
variantes sintéticas não representam a política atual completa.

### Parquet: comparação das políticas de alocação

`python scripts/benchmark_parquet_allocator.py` gera um milhão de linhas
aninhadas em 100 row groups, compila versões temporárias antes/depois das
mudanças de alocação e mede leitura e leitura+regravação em linhas e colunar.
Requer PyArrow; `--rows`, `--row-group-size` e `--repetitions` ajustam a carga.
Todas as regravações são comparadas integralmente por coluna com a entrada.
Metadados e obrigatoriedade dos campos de topo podem diferir; valores, nulos,
ordem e tipos são conferidos. Veja [resultados e limites](parquet-allocator-2026-09-26.md).

`python scripts/benchmark_parquet_large.py build/release/bin/tilt` compara o
scratch de páginas habilitado e desabilitado no mesmo binário em 1 milhão de
linhas, nos codecs gzip, snappy e zstd, medindo leitura, roundtrip, RSS e pico
por row group. Veja [a rodada de 2026-09-28](parquet-large-2026-09-28.md).
