#!/usr/bin/env python3
"""Comparable matmul/conv2d benchmark: Tilt, NumPy and optional PyTorch."""

from __future__ import annotations

import argparse
import json
import os
import shlex
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Callable

ROOT = Path(__file__).resolve().parents[1]


def measure(fn: Callable[[], object], warmup: int, iterations: int,
            checksum: Callable[[object], float]):
    for _ in range(warmup):
        checksum(fn())
    samples = []
    total = 0.0
    for _ in range(iterations):
        start = time.perf_counter_ns()
        result = fn()
        total += checksum(result)
        samples.append((time.perf_counter_ns() - start) / 1_000_000.0)
    samples.sort()
    middle = len(samples) // 2
    median = ((samples[middle - 1] + samples[middle]) / 2.0
              if len(samples) % 2 == 0 else samples[middle])
    return {"median_ms": median, "mean_ms": sum(samples) / len(samples),
            "checksum": total / iterations}


def compile_tilt() -> tuple[Path, tempfile.TemporaryDirectory]:
    compiler = shlex.split(os.environ.get("CXX", "c++"))
    if not compiler or shutil.which(compiler[0]) is None:
        raise RuntimeError("C++ compiler not found; set CXX")
    temp = tempfile.TemporaryDirectory(prefix="tilt-tensor-bench-")
    binary = Path(temp.name) / "tensor_ops_benchmark"
    command = compiler + [
        "-O3", "-DNDEBUG", "-std=c++20", "-Wall", "-Wextra", "-Wpedantic",
        "-I", str(ROOT / "src"),
        str(ROOT / "benchmarks" / "tensor_ops_benchmark.cpp"),
        str(ROOT / "src" / "runtime" / "tensor.cpp"),
        "-pthread", "-o", str(binary),
    ]
    try:
        subprocess.run(command, check=True)
    except Exception:
        temp.cleanup()
        raise
    return binary, temp


def run_tilt(iterations: int, warmup: int, quick: bool) -> list[dict]:
    binary, temp = compile_tilt()
    try:
        command = [str(binary), "--iterations", str(iterations), "--warmup", str(warmup)]
        if quick:
            command.append("--quick")
        output = subprocess.check_output(command, text=True)
        return [json.loads(line) for line in output.splitlines() if line.strip()]
    finally:
        temp.cleanup()


def arrays(numpy, quick: bool):
    matrix = 64 if quick else 256
    cin = 4 if quick else 16
    cout = 8 if quick else 32
    height = 16 if quick else 64
    width = 16 if quick else 64

    def values(size: int, period: int, scale: float):
        return ((numpy.arange(size, dtype=numpy.float32) % period) - period // 2) * scale

    mat_a = values(matrix * matrix, 17, 0.01).reshape(matrix, matrix)
    mat_b = values(matrix * matrix, 23, 0.007).reshape(matrix, matrix)
    conv_x = values(cin * height * width, 19, 0.01).reshape(1, cin, height, width)
    conv_k = values(cout * cin * 3 * 3, 13, 0.005).reshape(cout, cin, 3, 3)
    return mat_a, mat_b, conv_x, conv_k


def run_numpy(iterations: int, warmup: int, quick: bool) -> list[dict]:
    try:
        import numpy
    except ImportError:
        raise RuntimeError("NumPy not installed")
    mat_a, mat_b, conv_x, conv_k = arrays(numpy, quick)
    results = []
    matmul = measure(lambda: numpy.matmul(mat_a, mat_b), warmup, iterations,
                     lambda value: float(value.sum(dtype=numpy.float64)))
    results.append({"backend": "numpy", "operation": "matmul", **matmul})
    windows = numpy.lib.stride_tricks.sliding_window_view(conv_x, (3, 3), axis=(2, 3))

    def conv2d():
        return numpy.einsum("nchwkl,ockl->nohw", windows, conv_k, optimize=True)

    conv = measure(conv2d, warmup, iterations,
                   lambda value: float(value.sum(dtype=numpy.float64)))
    results.append({"backend": "numpy", "operation": "conv2d", **conv})
    return results


def run_torch(iterations: int, warmup: int, quick: bool) -> list[dict]:
    try:
        import torch
    except ImportError:
        raise RuntimeError("PyTorch not installed")
    import numpy
    mat_a, mat_b, conv_x, conv_k = arrays(numpy, quick)
    ta, tb = torch.from_numpy(mat_a), torch.from_numpy(mat_b)
    tx, tk = torch.from_numpy(conv_x), torch.from_numpy(conv_k)
    results = []
    with torch.no_grad():
        matmul = measure(lambda: torch.matmul(ta, tb), warmup, iterations,
                         lambda value: float(value.sum().item()))
        results.append({"backend": "torch", "operation": "matmul", **matmul})
        conv = measure(lambda: torch.nn.functional.conv2d(tx, tk), warmup, iterations,
                       lambda value: float(value.sum().item()))
        results.append({"backend": "torch", "operation": "conv2d", **conv})
    return results


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--iterations", type=int, default=20)
    parser.add_argument("--warmup", type=int, default=5)
    parser.add_argument("--quick", action="store_true", help="small dimensions for smoke test")
    parser.add_argument("--backend", choices=("all", "tilt", "numpy", "torch"), default="all",
                        help="backend to run; all tries NumPy/PyTorch when installed")
    parser.add_argument("--json", action="store_true", help="print JSON instead of a table")
    args = parser.parse_args()
    if args.iterations < 1 or args.warmup < 0:
        parser.error("--iterations must be >= 1 and --warmup must be >= 0")

    runners = {"tilt": run_tilt, "numpy": run_numpy, "torch": run_torch}
    selected = ("tilt", "numpy", "torch") if args.backend == "all" else (args.backend,)
    results = []
    for backend in selected:
        try:
            results.extend(runners[backend](args.iterations, args.warmup, args.quick))
        except RuntimeError as error:
            if args.backend != "all":
                print(f"error: {error}", file=sys.stderr)
                return 2
            print(f"[skipped] {backend}: {error}", file=sys.stderr)

    if not results:
        print("no backend available", file=sys.stderr)
        return 2
    if args.json:
        print(json.dumps(results, indent=2))
    else:
        print("backend operation median_ms mean_ms checksum")
        for result in results:
            print(f"{result['backend']} {result['operation']} "
                  f"{result['median_ms']:.4f} {result['mean_ms']:.4f} "
                  f"{result['checksum']:.6g}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
