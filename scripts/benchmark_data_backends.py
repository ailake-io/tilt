#!/usr/bin/env python3
"""Benchmark comparable CSV analytics in Tilt, pandas, Polars and DuckDB.

The optional Python backends are imported only when installed.  Missing
backends are reported explicitly on stderr, never as zero timings.  Workloads
cover grouped aggregation, filtered aggregation and a keyed join; CSV data
generation and imports are outside the timed sections.
"""

from __future__ import annotations

import argparse
import csv
import importlib.util
import json
import statistics
import subprocess
import tempfile
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def median_ms(fn, repetitions: int) -> float:
    samples = []
    for _ in range(repetitions):
        start = time.perf_counter_ns()
        fn()
        samples.append((time.perf_counter_ns() - start) / 1_000_000)
    return statistics.median(samples)


def make_data(directory: Path, rows: int) -> None:
    regions = ["norte", "sul", "leste", "oeste", "centro"]
    with (directory / "vendas.csv").open("w", newline="") as out:
        writer = csv.writer(out)
        writer.writerow(["id", "regiao", "valor"])
        for i in range(rows):
            writer.writerow([i, regions[i % len(regions)], (i * 17) % 500])
    with (directory / "regioes.csv").open("w", newline="") as out:
        writer = csv.writer(out)
        writer.writerow(["regiao", "meta"])
        for i, region in enumerate(regions):
            writer.writerow([region, 100 + i])


def write_tilt_programs(directory: Path) -> None:
    common = 'v = ler_csv "vendas.csv", colunar: verdadeiro\n'
    (directory / "group.tilt").write_text(
        "pipeline bench:\n  passos:\n"
        f"    - {common}    - g = v.agrupar_por \"regiao\", {{ total: somar \"valor\", n: contar }}\n"
        f"    - imprimir tamanho(g)\n"
    )
    (directory / "filter.tilt").write_text(
        "pipeline bench:\n  passos:\n"
        f"    - {common}    - f = v.filtrar linha.valor >= 250\n"
        f"    - g = f.agrupar_por \"regiao\", {{ total: somar \"valor\", n: contar }}\n"
        f"    - imprimir tamanho(g)\n"
    )
    (directory / "join.tilt").write_text(
        "pipeline bench:\n  passos:\n"
        f"    - {common}    - r = ler_csv \"regioes.csv\", colunar: verdadeiro\n"
        f"    - j = v.juntar r, por: \"regiao\"\n"
        f"    - imprimir tamanho(j)\n"
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("tilt", type=Path)
    parser.add_argument("--rows", type=int, default=200_000)
    parser.add_argument("--repetitions", type=int, default=5)
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()
    if args.rows < 1 or args.repetitions < 1:
        parser.error("--rows and --repetitions must be >= 1")

    available = {name: importlib.util.find_spec(name) is not None
                 for name in ("pandas", "polars", "duckdb")}
    missing = [name for name, present in available.items() if not present]
    if missing:
        print("backends_unavailable=" + ",".join(missing), file=__import__("sys").stderr)

    with tempfile.TemporaryDirectory(prefix="tilt-data-backends-") as tmp:
        work = Path(tmp)
        make_data(work, args.rows)
        write_tilt_programs(work)

        tilt_bin = str(args.tilt.resolve())
        regions = ["norte", "sul", "leste", "oeste", "centro"]
        filter_groups = len({regions[i % len(regions)] for i in range(args.rows)
                             if (i * 17) % 500 >= 250})

        def tilt_run(program: str):
            result = subprocess.run([tilt_bin, "executar", program], cwd=work,
                                    capture_output=True, text=True, check=True)
            if program == "join.tilt":
                expected = str(args.rows)
            elif program == "filter.tilt":
                expected = str(filter_groups)
            else:
                expected = str(min(args.rows, len(regions)))
            if expected not in result.stdout:
                raise RuntimeError(f"unexpected Tilt result for {program}: {result.stdout}")

        python_runs = {}
        if available["pandas"]:
            import pandas as pd

            def pandas_group():
                frame = pd.read_csv(work / "vendas.csv")
                frame.groupby("regiao", sort=False).agg(total=("valor", "sum"),
                                                         n=("valor", "size"))

            def pandas_filter():
                frame = pd.read_csv(work / "vendas.csv")
                frame.loc[frame["valor"] >= 250].groupby("regiao", sort=False).agg(
                    total=("valor", "sum"), n=("valor", "size"))

            def pandas_join():
                left = pd.read_csv(work / "vendas.csv")
                right = pd.read_csv(work / "regioes.csv")
                if len(left.merge(right, on="regiao")) != args.rows:
                    raise RuntimeError("pandas join result mismatch")

            python_runs["pandas"] = {"group": pandas_group, "filter": pandas_filter,
                                     "join": pandas_join}

        if available["polars"]:
            import polars as pl

            def polars_group():
                (pl.scan_csv(work / "vendas.csv")
                 .group_by("regiao")
                 .agg(pl.col("valor").sum().alias("total"), pl.len().alias("n"))
                 .collect(engine="streaming"))

            def polars_filter():
                (pl.scan_csv(work / "vendas.csv")
                 .filter(pl.col("valor") >= 250)
                 .group_by("regiao")
                 .agg(pl.col("valor").sum().alias("total"), pl.len().alias("n"))
                 .collect(engine="streaming"))

            def polars_join():
                left = pl.scan_csv(work / "vendas.csv")
                right = pl.scan_csv(work / "regioes.csv")
                if left.join(right, on="regiao", how="inner").select(pl.len()).collect().item() != args.rows:
                    raise RuntimeError("polars join result mismatch")

            python_runs["polars"] = {"group": polars_group, "filter": polars_filter,
                                     "join": polars_join}

        if available["duckdb"]:
            import duckdb

            def duckdb_group():
                con = duckdb.connect()
                con.execute("SELECT regiao, sum(valor), count(*) FROM read_csv_auto(?) GROUP BY regiao",
                            [str(work / "vendas.csv")])
                con.close()

            def duckdb_filter():
                con = duckdb.connect()
                con.execute("SELECT regiao, sum(valor), count(*) FROM read_csv_auto(?) "
                            "WHERE valor >= 250 GROUP BY regiao", [str(work / "vendas.csv")])
                con.close()

            def duckdb_join():
                con = duckdb.connect()
                count = con.execute("SELECT count(*) FROM read_csv_auto(?) v "
                                    "JOIN read_csv_auto(?) r USING (regiao)",
                                    [str(work / "vendas.csv"), str(work / "regioes.csv")]).fetchone()[0]
                con.close()
                if count != args.rows:
                    raise RuntimeError("duckdb join result mismatch")

            python_runs["duckdb"] = {"group": duckdb_group, "filter": duckdb_filter,
                                     "join": duckdb_join}

        # One validation run per backend is outside the timing loop.
        tilt_run("group.tilt")
        for runs in python_runs.values():
            for run in runs.values():
                run()

        records = []
        for backend, runs in [("tilt", {"group": lambda: tilt_run("group.tilt"),
                                         "filter": lambda: tilt_run("filter.tilt"),
                                         "join": lambda: tilt_run("join.tilt")})] + list(python_runs.items()):
            for operation, run in runs.items():
                records.append({"backend": backend, "operation": operation,
                                "rows": args.rows,
                                "median_ms": round(median_ms(run, args.repetitions), 3)})

    if args.json:
        print(json.dumps(records, ensure_ascii=False))
    else:
        print("backend,operation,rows,median_ms")
        for record in records:
            print("{backend},{operation},{rows},{median_ms:.3f}".format(**record))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
