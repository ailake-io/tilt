#!/usr/bin/env python3
"""Benchmark do hash join colunar, incluindo reutilização do índice da direita."""

from __future__ import annotations

import argparse
import csv
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
        parser.error("rows e repetitions devem ser positivos")

    with tempfile.TemporaryDirectory(prefix="tilt-join-benchmark-") as tmp:
        work = Path(tmp)
        with (work / "esquerda.csv").open("w", newline="") as handle:
            writer = csv.writer(handle)
            writer.writerow(("id", "valor"))
            for row in range(args.rows):
                writer.writerow((row, row * 2))
        with (work / "direita.csv").open("w", newline="") as handle:
            writer = csv.writer(handle)
            writer.writerow(("id", "categoria"))
            for row in range(args.rows):
                writer.writerow((row, row % 17))

        binary = str(args.tilt.resolve())
        samples: dict[str, list[float]] = {"linhas": [], "colunar": []}
        for mode, option in (("linhas", "falso"), ("colunar", "verdadeiro")):
            program = work / f"{mode}.tilt"
            program.write_text(f'''pipeline join:
  passos:
    - esquerda = ler_csv "esquerda.csv", colunar: {option}
    - direita = ler_csv "direita.csv", colunar: {option}
    - primeiro = esquerda.juntar direita, por: "id"
    - segundo = esquerda.juntar direita, por: "id"
    - imprimir tamanho(primeiro), tamanho(segundo)
''')
            command = [binary, "executar", program.name]
            for _ in range(args.repetitions):
                start = time.perf_counter_ns()
                result = subprocess.run(command, cwd=work, capture_output=True, text=True)
                elapsed = (time.perf_counter_ns() - start) / 1_000_000
                if result.returncode != 0:
                    raise RuntimeError(result.stderr)
                if result.stdout.strip().splitlines()[-1].split() != [str(args.rows), str(args.rows)]:
                    raise RuntimeError("resultado do join incorreto")
                samples[mode].append(elapsed)
        for mode in ("linhas", "colunar"):
            median = statistics.median(samples[mode])
            print(f"{mode},rows={args.rows},median_ms={median:.3f}")
        speedup = statistics.median(samples["linhas"]) / statistics.median(samples["colunar"])
        print(f"speedup_colunar={speedup:.3f}x")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
