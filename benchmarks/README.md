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
