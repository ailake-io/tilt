#!/usr/bin/env python3
"""Compare Parquet scratch reuse on million-row nested files and codecs."""

from __future__ import annotations

import argparse
import json
import platform
import statistics
import subprocess
import tempfile
import time
from pathlib import Path


def run_tilt(binary: Path, work: Path, program: str, env: dict[str, str],
             rows: int, columnar: bool, roundtrip: bool) -> dict:
    (work / "run.tilt").write_text(program, encoding="utf-8")
    output = work / "output.parquet"
    if output.exists():
        output.unlink()
    rss_file = work / "rss.txt"
    started = time.perf_counter_ns()
    completed = subprocess.run(
        ["/usr/bin/time", "-f", "%M", "-o", str(rss_file), str(binary),
         "executar", "run.tilt"],
        cwd=work, env={**__import__("os").environ, **env},
        capture_output=True, text=True, check=True,
    )
    elapsed_ms = (time.perf_counter_ns() - started) / 1_000_000
    lines = [line for line in completed.stdout.splitlines()
             if not line.startswith("== pipeline ")]
    if not lines or int(lines[0]) != rows:
        raise RuntimeError(f"contagem inesperada: {completed.stdout!r}")
    peak = int(lines[1]) if columnar else None
    return {"ms": elapsed_ms, "peak_rss_kib": int(rss_file.read_text()),
            "peak_row_group_bytes": peak}


def validate(reference, path: Path) -> None:
    import pyarrow.parquet as pq

    actual = pq.read_table(path).select(reference.column_names)
    if actual.num_rows != reference.num_rows:
        raise RuntimeError(f"roundtrip com {actual.num_rows} linhas; esperado {reference.num_rows}")
    for name in reference.column_names:
        if not actual.column(name).equals(reference.column(name)):
            raise RuntimeError(f"roundtrip divergente na coluna {name}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("tilt", type=Path)
    parser.add_argument("--rows", type=int, default=1_000_000)
    parser.add_argument("--row-group-size", type=int, default=10_000)
    parser.add_argument("--repetitions", type=int, default=1)
    parser.add_argument("--codecs", default="gzip,snappy,zstd")
    args = parser.parse_args()
    if min(args.rows, args.row_group_size, args.repetitions) < 1:
        parser.error("rows, row-group-size e repetitions devem ser positivos")
    codecs = [codec.strip() for codec in args.codecs.split(",") if codec.strip()]
    if not codecs or any(codec not in {"gzip", "snappy", "zstd"} for codec in codecs):
        parser.error("codecs deve conter apenas gzip,snappy,zstd")

    import pyarrow as pa
    import pyarrow.parquet as pq

    binary = args.tilt.resolve()
    records = []
    with tempfile.TemporaryDirectory(prefix="tilt-parquet-large-") as directory:
        work = Path(directory)
        ids = list(range(args.rows))
        table = pa.table({
            "id": pa.array(ids, type=pa.int64()),
            "nums": pa.array(
                [None if i % 11 == 0 else [] if i % 7 == 0
                 else [i, None, i % 17, i + 1] for i in ids],
                type=pa.list_(pa.int64()),
            ),
            "meta": pa.array(
                [None if i % 13 == 0 else
                 {"bucket": i % 5,
                  "label": None if i % 19 == 0 else f"label-{i % 997}"}
                 for i in ids],
                type=pa.struct([("bucket", pa.int64()), ("label", pa.string())]),
            ),
        })
        source = work / "input.parquet"
        reference = table
        for codec in codecs:
            pq.write_table(table, source, row_group_size=args.row_group_size,
                           compression=codec, use_dictionary=True)
            file_info = {"bytes": source.stat().st_size,
                         "row_groups": pq.ParquetFile(source).num_row_groups}
            for variant, variant_env in (("baseline", {"TILT_PARQUET_SCRATCH": "0"}),
                                         ("scratch", {})):
                for mode, columnar in (("rows", False), ("columnar", True)):
                    option = ", colunar: verdadeiro" if columnar else ""
                    peak = "    - imprimir dados.metricas_memoria().pico_row_group_bytes\n" if columnar else ""
                    for operation, roundtrip in (("read", False), ("roundtrip", True)):
                        write = (f'    - escrever_parquet dados, "output.parquet", '
                                 f'codec: "{codec}", row_group: {args.row_group_size}\n'
                                 if roundtrip else "")
                        program = ("pipeline benchmark:\n  passos:\n"
                                   f'    - dados = ler_parquet "input.parquet"{option}\n'
                                   "    - imprimir tamanho(dados)\n" + peak + write)
                        samples = []
                        for _ in range(args.repetitions):
                            sample = run_tilt(binary, work, program, variant_env,
                                              args.rows, columnar, roundtrip)
                            if roundtrip:
                                validate(reference, work / "output.parquet")
                            samples.append(sample)
                        result = {
                            "median_ms": statistics.median(s["ms"] for s in samples),
                            "median_peak_rss_kib": statistics.median(s["peak_rss_kib"] for s in samples),
                            "median_peak_row_group_bytes": statistics.median(
                                s["peak_row_group_bytes"] for s in samples
                                if s["peak_row_group_bytes"] is not None
                            ) if columnar else None,
                            "samples": samples,
                        }
                        records.append({"codec": codec, "variant": variant,
                                        "mode": mode, "operation": operation,
                                        "input": file_info, "result": result})
                        print(f"completed {codec}/{variant}/{mode}/{operation}",
                              file=__import__("sys").stderr, flush=True)
    config = vars(args).copy()
    config["tilt"] = str(config["tilt"])
    report = {"config": config, "platform": platform.platform(),
              "pyarrow": pa.__version__, "results": records}
    print(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
