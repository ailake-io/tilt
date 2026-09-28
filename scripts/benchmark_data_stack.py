#!/usr/bin/env python3
"""Compare Tilt and pandas on the same CSV -> groupby -> Parquet workload.

Generate bench/vendas.csv once with bench/gerar_csv.py. The timed section
includes read, aggregation, and Parquet write, but excludes data generation,
imports, and result validation. Run both implementations on the same machine.
"""

from __future__ import annotations

import argparse
import importlib.util
import shutil
import subprocess
import tempfile
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def median_ms(fn, repetitions: int) -> float:
    values = []
    for _ in range(repetitions):
        start = time.perf_counter_ns()
        fn()
        values.append((time.perf_counter_ns() - start) / 1_000_000)
    return sorted(values)[len(values) // 2]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("tilt", type=Path)
    parser.add_argument("--repetitions", type=int, default=5)
    parser.add_argument("--row-group-size", type=int, default=0,
                        help="use multi-row-group Parquet writing (0 keeps one group)")
    args = parser.parse_args()
    if args.repetitions < 1:
        parser.error("--repetitions must be >= 1")
    if args.row_group_size < 0:
        parser.error("--row-group-size must be >= 0")
    try:
        import pandas as pd
    except ImportError as exc:
        parser.error(f"pandas is required: {exc}")

    source = ROOT / "bench" / "vendas.csv"
    if not source.exists():
        subprocess.run(["python3", str(ROOT / "bench" / "gerar_csv.py")], check=True)
    with source.open("rb") as source_file:
        row_count = max(0, sum(1 for _ in source_file) - 1)
    with tempfile.TemporaryDirectory(prefix="tilt-data-benchmark-") as tmp:
        work = Path(tmp)
        row_group_kw = f", row_group: {args.row_group_size}" if args.row_group_size else ""
        shutil.copyfile(source, work / "vendas.csv")
        shutil.copyfile(ROOT / "bench" / "dados.tilt", work / "dados.tilt")
        (work / "dados_projetados.tilt").write_text(f'''pipeline agregar:
  passos:
    - t = ler_csv "vendas.csv", selecionar: ["regiao", "valor"]
    - imprimir "linhas:", tamanho(t)
    - r = t.agrupar_por "regiao", {{ total: somar "valor", n: contar }}
    - escrever_parquet r, "saida_projetada.parquet", codec: "gzip"{row_group_kw}
    - imprimir r
''')
        (work / "dados_colunares.tilt").write_text(f'''pipeline agregar:
  passos:
    - t = ler_csv "vendas.csv", selecionar: ["regiao", "valor"], colunar: verdadeiro
    - imprimir "linhas:", tamanho(t)
    - r = t.agrupar_por "regiao", {{ total: somar "valor", n: contar }}
    - escrever_parquet r, "saida_colunar.parquet", codec: "gzip"{row_group_kw}
    - imprimir r
''')
        for mode, option in (("linhas", ""), ("colunas", ", colunar: verdadeiro")):
            (work / f"filtrado_{mode}.tilt").write_text(f'''pipeline agregar:
  passos:
    - t = ler_csv "vendas.csv", selecionar: ["regiao", "valor"]{option}
    - f = t.filtrar linha.valor >= 250
    - r = f.agrupar_por "regiao", {{ total: somar "valor", n: contar }}
    - escrever_parquet r, "filtrado_{mode}.parquet", codec: "gzip"{row_group_kw}
    - imprimir r
''')

        def tilt_run(file="dados.tilt"):
            subprocess.run([str(args.tilt.resolve()), "executar", file],
                           cwd=work, check=True, capture_output=True)

        def pandas_run(projected=False):
            frame = pd.read_csv(work / "vendas.csv", usecols=["regiao", "valor"] if projected else None)
            grouped = frame.groupby("regiao", sort=False, as_index=False).agg(
                total=("valor", "sum"), n=("valor", "size"))
            path = "pandas_projetado.parquet" if projected else "pandas.parquet"
            grouped.to_parquet(work / path, index=False, compression="gzip")

        def pandas_filtered():
            frame = pd.read_csv(work / "vendas.csv", usecols=["regiao", "valor"])
            grouped = frame.loc[frame["valor"] >= 250].groupby(
                "regiao", sort=False, as_index=False).agg(total=("valor", "sum"), n=("valor", "size"))
            grouped.to_parquet(work / "pandas_filtrado.parquet", index=False, compression="gzip")

        optional = {}
        unavailable = []
        if importlib.util.find_spec("polars"):
            import polars as pl

            def polars_run():
                grouped = (pl.scan_csv(work / "vendas.csv")
                           .group_by("regiao")
                           .agg(pl.col("valor").sum().alias("total"), pl.len().alias("n"))
                           .collect(engine="streaming"))
                grouped.write_parquet(work / "polars.parquet", compression="gzip")

            optional["polars"] = (polars_run, "polars.parquet")
        else:
            unavailable.append("polars")
        if importlib.util.find_spec("duckdb"):
            import duckdb

            def duckdb_run():
                con = duckdb.connect()
                con.execute("COPY (SELECT regiao, sum(valor) AS total, count(*) AS n "
                            "FROM read_csv_auto(?) GROUP BY regiao) TO ? "
                            "(FORMAT PARQUET, COMPRESSION GZIP)",
                            [str(work / "vendas.csv"), str(work / "duckdb.parquet")])
                con.close()

            optional["duckdb"] = (duckdb_run, "duckdb.parquet")
        else:
            unavailable.append("duckdb")

        tilt_run()
        pandas_run()
        tilt_run("dados_projetados.tilt")
        tilt_run("dados_colunares.tilt")
        pandas_run(projected=True)
        tilt_run("filtrado_linhas.tilt")
        tilt_run("filtrado_colunas.tilt")
        pandas_filtered()
        for run, _ in optional.values():
            run()
        tilt_result = pd.read_parquet(work / "saida.parquet").sort_values("regiao")
        for backend, path in (("pandas", "pandas.parquet"),
                              ("tilt_projected", "saida_projetada.parquet"),
                              ("tilt_columnar", "saida_colunar.parquet"),
                              ("pandas_projected", "pandas_projetado.parquet"),
                              *((name, file) for name, (_, file) in optional.items())):
            result = pd.read_parquet(work / path).sort_values("regiao")
            for key in ("regiao", "total", "n"):
                if tilt_result[key].tolist() != result[key].tolist():
                    raise RuntimeError(f"Tilt and {backend} disagree on {key}")

        filtered = pd.read_parquet(work / "pandas_filtrado.parquet").sort_values("regiao")
        for backend in ("linhas", "colunas"):
            result = pd.read_parquet(work / f"filtrado_{backend}.parquet").sort_values("regiao")
            for key in ("regiao", "total", "n"):
                if filtered[key].tolist() != result[key].tolist():
                    raise RuntimeError(f"filtered Tilt {backend} and pandas disagree on {key}")

        results = [("tilt", "csv_groupby_parquet", median_ms(tilt_run, args.repetitions)),
                   ("tilt", "csv_projected_groupby_parquet",
                    median_ms(lambda: tilt_run("dados_projetados.tilt"), args.repetitions)),
                   ("tilt", "csv_columnar_groupby_parquet",
                    median_ms(lambda: tilt_run("dados_colunares.tilt"), args.repetitions)),
                   ("pandas", "csv_groupby_parquet", median_ms(pandas_run, args.repetitions)),
                   ("pandas", "csv_projected_groupby_parquet",
                    median_ms(lambda: pandas_run(True), args.repetitions)),
                   ("tilt", "csv_filtered_rows_groupby_parquet",
                    median_ms(lambda: tilt_run("filtrado_linhas.tilt"), args.repetitions)),
                   ("tilt", "csv_filtered_columnar_groupby_parquet",
                    median_ms(lambda: tilt_run("filtrado_colunas.tilt"), args.repetitions)),
                   ("pandas", "csv_filtered_groupby_parquet",
                    median_ms(pandas_filtered, args.repetitions))]
        for backend, (run, _) in optional.items():
            results.append((backend, "csv_groupby_parquet", median_ms(run, args.repetitions)))
        print("backend,operation,rows,median_ms")
        for backend, operation, elapsed in results:
            print(f"{backend},{operation},{row_count},{elapsed:.3f}")
        if unavailable:
            print("backends_unavailable=" + ",".join(unavailable), file=__import__("sys").stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
