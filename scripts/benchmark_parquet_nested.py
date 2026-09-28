#!/usr/bin/env python3
"""Measure Parquet list/struct reads across row groups in Tilt row/column modes."""

from __future__ import annotations

import argparse
import statistics
import subprocess
import tempfile
import time
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("tilt", type=Path)
    parser.add_argument("--rows", type=int, default=100_000)
    parser.add_argument("--repetitions", type=int, default=3)
    args = parser.parse_args()
    if args.rows < 1 or args.repetitions < 1:
        parser.error("--rows and --repetitions must be positive")
    import pyarrow as pa
    import pyarrow.parquet as pq

    with tempfile.TemporaryDirectory(prefix="tilt-nested-benchmark-") as tmp:
        work = Path(tmp)
        n = args.rows
        table = pa.table({
            "nums": pa.array([None if i % 11 == 0 else [i % 7, (i + 1) % 7]
                              for i in range(n)], type=pa.list_(pa.int64())),
            "meta": pa.array([None if i % 13 == 0 else
                              {"bucket": i % 5, "label": f"b{i % 5}"}
                              for i in range(n)],
                             type=pa.struct([("bucket", pa.int64()),
                                             ("label", pa.string())])),
        })
        pq.write_table(table, work / "nested.parquet", row_group_size=5_000,
                       compression="gzip")
        for mode, option in (("rows", ""), ("columnar", ", colunar: verdadeiro")):
            metricas = "    - m = dados.metricas_memoria()\n    - imprimir m.pico_row_group_bytes\n" if mode == "columnar" else ""
            (work / f"{mode}.tilt").write_text(f'''pipeline leitura:
  passos:
    - dados = ler_parquet "nested.parquet"{option}
    - imprimir tamanho(dados)
{metricas}''')

        print("mode,median_ms,peak_kib")
        for mode in ("rows", "columnar"):
            times = []
            peaks = []
            row_group_peaks = []
            for _ in range(args.repetitions):
                start = time.perf_counter_ns()
                result = subprocess.run(
                    ["/usr/bin/time", "-f", "%M", str(args.tilt.resolve()),
                     "executar", f"{mode}.tilt"],
                    cwd=work, capture_output=True, text=True, check=True)
                times.append((time.perf_counter_ns() - start) / 1_000_000)
                peaks.append(int(result.stderr.strip()))
                linhas = result.stdout.splitlines()
                if linhas[-2 if mode == "columnar" else -1] != str(n):
                    raise RuntimeError(f"{mode}: row count mismatch")
                if mode == "columnar":
                    # A métrica é uma linha inteira porque o programa imprime
                    # o campo escalar diretamente.
                    row_group_peaks.append(int(linhas[-1]))
            extra = f",row_group_peak_bytes={statistics.median(row_group_peaks):.0f}" if mode == "columnar" else ""
            print(f"{mode},{statistics.median(times):.3f},{statistics.median(peaks):.0f}{extra}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
