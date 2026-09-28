#!/usr/bin/env python3
"""Compare Parquet read + groupby in Tilt row/column modes and pandas.

Pandas import and input creation are excluded from timing. Each Tilt sample
starts a new process; pandas runs in the already loaded Python process.
"""

from __future__ import annotations

import argparse
import statistics
import subprocess
import tempfile
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("tilt", type=Path)
    parser.add_argument("--repetitions", type=int, default=7)
    parser.add_argument("--plain", action="store_true",
                        help="disable Parquet dictionary encoding")
    args = parser.parse_args()
    if args.repetitions < 1:
        parser.error("--repetitions must be >= 1")

    import pandas as pd

    source = ROOT / "bench" / "vendas.csv"
    if not source.exists():
        subprocess.run(["python3", str(ROOT / "bench" / "gerar_csv.py")], check=True)
    frame = pd.read_csv(source, usecols=["regiao", "valor"])
    expected = frame.groupby("regiao", sort=False, as_index=False).agg(
        total=("valor", "sum"), n=("valor", "size"))
    expected_rows = [(r.regiao, int(r.total), int(r.n))
                     for r in expected.itertuples(index=False)]

    with tempfile.TemporaryDirectory(prefix="tilt-parquet-benchmark-") as tmp:
        work = Path(tmp)
        path = work / "entrada.parquet"
        frame.to_parquet(path, compression="gzip", index=False,
                         use_dictionary=not args.plain)
        for mode, option in (("rows", ""), ("columnar", ", colunar: verdadeiro")):
            (work / f"{mode}.tilt").write_text(f'''pipeline agregar:
  passos:
    - dados = ler_parquet "entrada.parquet", selecionar: ["regiao", "valor"]{option}
    - grupos = dados.agrupar_por "regiao", {{ total: somar "valor", n: contar }}
    - para cada grupo em grupos:
        - imprimir grupo.regiao, grupo.total, grupo.n
''')

        def tilt_run(mode: str) -> None:
            result = subprocess.run(
                [str(args.tilt.resolve()), "executar", f"{mode}.tilt"],
                cwd=work, capture_output=True, text=True, check=True)
            rows = [line.split() for line in result.stdout.splitlines()[1:]]
            actual = [(name, int(total), int(n)) for name, total, n in rows]
            if actual != expected_rows:
                raise RuntimeError(f"{mode}: resultado diferente do pandas")

        def pandas_run() -> None:
            data = pd.read_parquet(path, columns=["regiao", "valor"])
            grouped = data.groupby("regiao", sort=False, as_index=False).agg(
                total=("valor", "sum"), n=("valor", "size"))
            actual = [(r.regiao, int(r.total), int(r.n))
                      for r in grouped.itertuples(index=False)]
            if actual != expected_rows:
                raise RuntimeError("pandas: resultado incorreto")

        runners = {"tilt_rows": lambda: tilt_run("rows"),
                   "tilt_columnar": lambda: tilt_run("columnar"),
                   "pandas": pandas_run}
        for run in runners.values():
            run()
        samples: dict[str, list[float]] = {name: [] for name in runners}
        for _ in range(args.repetitions):
            for name, run in runners.items():
                start = time.perf_counter_ns()
                run()
                samples[name].append((time.perf_counter_ns() - start) / 1_000_000)

        print(f"rows={len(frame)}, groups={len(expected_rows)}, "
              f"sum={expected.total.sum()}, encoding={'plain' if args.plain else 'dictionary'}")
        print("backend,median_ms")
        for name, times in samples.items():
            print(f"{name},{statistics.median(times):.3f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
