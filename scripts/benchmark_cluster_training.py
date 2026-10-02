#!/usr/bin/env python3
"""Measure 1/2/4 local Parquet training ranks and checkpoint synchronization.

This uses one host and a local shared directory. It does not measure network
filesystem latency or multi-host training throughput.
"""

from __future__ import annotations

import argparse
import os
import re
import statistics
import subprocess
import tempfile
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("tilt", type=Path)
    parser.add_argument("--rows", type=int, default=20_000)
    parser.add_argument("--repetitions", type=int, default=3)
    args = parser.parse_args()
    if args.rows < 16 or args.repetitions < 1:
        parser.error("--rows must be >= 16 and --repetitions >= 1")
    import pyarrow as pa
    import pyarrow.parquet as pq

    template = (ROOT / "tests/golden/run-treino-fluxo/input.tilt").read_text()
    template = template.replace("  epocas: 30\n", "  epocas: 2\n")
    template = template.replace("  lote: 4\n", "  lote: 64\n")
    template = template.replace("  bloco: 4\n", "  bloco: 1024\n")
    binary = str(args.tilt.resolve())
    env = dict(os.environ, TILT_GPU="off", TILT_BLAS="off", TILT_CLUSTER_PROFILE="1")

    with tempfile.TemporaryDirectory(prefix="tilt-cluster-benchmark-") as tmp:
        work = Path(tmp)
        n = args.rows
        pq.write_table(pa.table({
            "x1": pa.array([i % 11 for i in range(n)], type=pa.int64()),
            "x2": pa.array([(i * 3) % 13 for i in range(n)], type=pa.int64()),
            "y": pa.array([int((i % 11) + ((i * 3) % 13) > 10)
                           for i in range(n)], type=pa.int64()),
        }), work / "dados.parquet", row_group_size=max(1, n // 16),
            compression="gzip", use_dictionary=False)
        source = template.replace('"dados.csv"', '"' + str(work / "dados.parquet") + '"')

        def run(world: int, repetition: int) -> tuple[float, float, float]:
            cluster = work / f"cluster-{world}-{repetition}"
            paths = []
            for rank in range(world):
                config = source
                if world > 1:
                    config = config.replace(
                        "  semente: 7\n",
                        "  semente: 7\n  cluster: { dir: \"" + str(cluster) +
                        "\", rank: " + str(rank) +
                        f", mundo: {world}, timeout: 30 }}\n")
                path = work / f"worker-{world}-{repetition}-{rank}.tilt"
                path.write_text(config)
                paths.append(path)

            start = time.perf_counter_ns()
            processes = [subprocess.Popen([binary, "executar", str(path)],
                                          cwd=work, env=env, stdout=subprocess.PIPE,
                                          stderr=subprocess.PIPE, text=True)
                         for path in paths]
            labels = []
            sync = []
            for process in processes:
                stdout, stderr = process.communicate()
                if process.returncode != 0 or "treino M:" not in stdout:
                    raise RuntimeError(f"worker failed ({process.returncode}): {stdout}\n{stderr}")
                found = re.search(r"tilt-profile labels_ms=([0-9.]+)", stderr)
                if not found:
                    raise RuntimeError(f"missing read profile: {stderr}")
                labels.append(float(found.group(1)))
                epochs = [float(value) for value in re.findall(
                    r"tilt-profile sync_epoch_ms=\d+,([0-9.]+)", stderr)]
                if len(epochs) != (2 if world > 1 else 0):
                    raise RuntimeError(f"missing sync profile: {stderr}")
                sync.append(sum(epochs))
            wall_ms = (time.perf_counter_ns() - start) / 1_000_000
            if world > 1 and not (cluster / "aggregate-epoch-2.json").is_file():
                raise RuntimeError("aggregate checkpoint missing")
            return wall_ms, max(labels), max(sync)

        print("workers,wall_ms,labels_ms,sync_ms,other_ms")
        for world in (1, 2, 4):
            samples = [run(world, repetition) for repetition in range(args.repetitions)]
            wall = statistics.median(s[0] for s in samples)
            labels = statistics.median(s[1] for s in samples)
            sync = statistics.median(s[2] for s in samples)
            print(f"{world},{wall:.3f},{labels:.3f},{sync:.3f},"
                  f"{wall - labels - sync:.3f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
