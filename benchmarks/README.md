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

### Sharding de treino

Um bloco treino pode selecionar uma particao deterministica das linhas com
num_shards: N e shard_id: K, usando o mesmo esquema round-robin nos dados em
RAM, no fluxo CSV e no fluxo Parquet. Cada processo/worker recebe K em
0..N-1; com num_shards: 1 o comportamento permanece o mesmo. A divisao de
validacao e feita dentro das linhas do shard.
