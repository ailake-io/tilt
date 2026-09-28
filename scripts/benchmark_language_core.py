#!/usr/bin/env python3
"""Compare Tilt interpreter/VM/JIT and CPython on identical numeric workloads."""

from __future__ import annotations

import argparse
import statistics
import subprocess
import sys
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]

PY_LOOP = """total = 0
i = 0
while i < 3000000:
    total = total + i * 2
    i = i + 1
print(total)
"""

PY_FIB = """def fib(n):
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)
print(fib(30))
"""


def measure(command: list[str], repetitions: int, expected: str) -> float:
    samples = []
    for _ in range(repetitions):
        begin = time.perf_counter_ns()
        result = subprocess.run(command, capture_output=True, text=True, check=True)
        samples.append((time.perf_counter_ns() - begin) / 1_000_000)
        if expected not in result.stdout:
            raise RuntimeError(f"unexpected result from {command}: {result.stdout}")
    return statistics.median(samples)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("tilt", type=Path)
    parser.add_argument("--repetitions", type=int, default=5)
    args = parser.parse_args()
    if args.repetitions < 1:
        parser.error("--repetitions must be >= 1")
    binary = str(args.tilt.resolve())
    print("backend,operation,median_ms")
    for operation, file, python_code, expected in (
        ("loop_3m", "laco.tilt", PY_LOOP, "8999997000000"),
        ("fib_30", "fib.tilt", PY_FIB, "832040"),
    ):
        path = str(ROOT / "bench" / file)
        for label, command in (
            ("tilt_interp", [binary, "executar", path]),
            ("tilt_vm", [binary, "executar", "--vm", path]),
            ("tilt_jit", [binary, "executar", "--jit", path]),
            ("cpython", [sys.executable, "-c", python_code]),
        ):
            ms = measure(command, args.repetitions, expected)
            print(f"{label},{operation},{ms:.3f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
