#!/usr/bin/env python3
"""Measure 1/2/4 local Tilt workers on a fixed Parquet groupby workload.

Each worker reads a distinct Parquet file. Input creation and pandas import
are excluded. This measures local multi-process scaling, not cluster networking.
"""

from __future__ import annotations

import argparse
from collections import defaultdict
import statistics
import subprocess
import tempfile
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("tilt", type=Path)
    parser.add_argument("--repetitions", type=int, default=3)
    args = parser.parse_args()
    if args.repetitions < 1:
        parser.error("--repetitions must be >= 1")
    import pandas as pd

    source = ROOT / "bench" / "vendas.csv"
    if not source.exists():
        subprocess.run(["python3", str(ROOT / "bench" / "gerar_csv.py")], check=True)
    frame = pd.read_csv(source, usecols=["regiao", "valor"])
    reference = frame.groupby("regiao", sort=False).agg(
        total=("valor", "sum"), n=("valor", "size"))
    expected = {name: (int(row.total), int(row.n))
                for name, row in reference.iterrows()}
    binary = str(args.tilt.resolve())

    with tempfile.TemporaryDirectory(prefix="tilt-distributed-benchmark-") as tmp:
        work = Path(tmp)
        for workers in (1, 2, 4):
            for rank in range(workers):
                start = len(frame) * rank // workers
                end = len(frame) * (rank + 1) // workers
                frame.iloc[start:end].to_parquet(
                    work / f"part-{workers}-{rank}.parquet", compression="gzip", index=False)
                for mode, option in (("rows", ""), ("columnar", ", colunar: verdadeiro")):
                    prefix = f'''pipeline agregar:
  passos:
    - dados = ler_parquet "part-{workers}-{rank}.parquet", selecionar: ["regiao", "valor"]{option}
'''
                    (work / f"{mode}-read-{workers}-{rank}.tilt").write_text(
                        prefix + "    - imprimir tamanho(dados)\n")
                    (work / f"{mode}-groupby-{workers}-{rank}.tilt").write_text(prefix + '''    - grupos = dados.agrupar_por "regiao", { total: somar "valor", n: contar }
    - para cada grupo em grupos:
        - imprimir grupo.regiao, grupo.total, grupo.n
''')

            def run(mode: str, stage: str) -> tuple[float, float]:
                start = time.perf_counter_ns()
                processes = [subprocess.Popen(
                    [binary, "executar", f"{mode}-{stage}-{workers}-{rank}.tilt"],
                    cwd=work, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                    for rank in range(workers)]
                outputs = []
                for process in processes:
                    stdout, stderr = process.communicate()
                    if process.returncode != 0:
                        raise RuntimeError(f"worker failed: {stderr}")
                    outputs.append(stdout)
                worker_ms = (time.perf_counter_ns() - start) / 1_000_000

                merge_start = time.perf_counter_ns()
                if stage == "read":
                    actual_rows = sum(int(output.splitlines()[-1]) for output in outputs)
                    if actual_rows != len(frame):
                        raise RuntimeError(f"{mode}/{workers}: row count mismatch")
                    return worker_ms, (time.perf_counter_ns() - merge_start) / 1_000_000
                merged: dict[str, list[int]] = defaultdict(lambda: [0, 0])
                for output in outputs:
                    for line in output.splitlines()[1:]:
                        name, total, n = line.split()
                        merged[name][0] += int(total)
                        merged[name][1] += int(n)
                actual = {name: tuple(values) for name, values in merged.items()}
                if actual != expected:
                    raise RuntimeError(f"{mode}/{workers}: aggregate mismatch")
                merge_ms = (time.perf_counter_ns() - merge_start) / 1_000_000
                return worker_ms, merge_ms

            stages = [(mode, stage) for mode in ("rows", "columnar")
                      for stage in ("read", "groupby")]
            for mode, stage in stages:
                run(mode, stage)  # warm page cache and validate before timing
            samples: dict[tuple[str, str], list[tuple[float, float]]] = {
                pair: [] for pair in stages}
            for _ in range(args.repetitions):
                for mode, stage in stages:
                    samples[(mode, stage)].append(run(mode, stage))
            for (mode, stage), results in samples.items():
                worker_ms = statistics.median(x for x, _ in results)
                merge_ms = statistics.median(x for _, x in results)
                print(f"{workers},{mode},{stage},{worker_ms:.3f},{merge_ms:.3f},"
                      f"{len(frame) / (worker_ms / 1000):.0f}")

    return 0


if __name__ == "__main__":
    print("workers,mode,stage,worker_wall_ms,merge_ms,rows_per_second")
    raise SystemExit(main())
