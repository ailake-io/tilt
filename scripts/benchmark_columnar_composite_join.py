#!/usr/bin/env python3
"""Compara merge join e hash join colunar em duas chaves."""

from __future__ import annotations

import argparse
import csv
import random
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

    with tempfile.TemporaryDirectory(prefix="tilt-composite-join-") as tmp:
        work = Path(tmp)
        chaves = [(row % 31, row) for row in range(args.rows)]
        for nome, valores in (("ordenada", chaves), ("embaralhada", random.Random(7).sample(chaves, len(chaves)))):
            with (work / f"direita_{nome}.csv").open("w", newline="") as handle:
                writer = csv.writer(handle)
                writer.writerow(("grupo", "id", "categoria"))
                for grupo, row in valores:
                    writer.writerow((grupo, row, row % 11))
        with (work / "esquerda.csv").open("w", newline="") as handle:
            writer = csv.writer(handle)
            writer.writerow(("grupo", "id", "valor"))
            for grupo, row in chaves:
                writer.writerow((grupo, row, row * 2))

        binary = str(args.tilt.resolve())
        samples: dict[str, list[float]] = {"merge": [], "hash": []}
        for mode, direita in (("merge", "ordenada"), ("hash", "embaralhada")):
            program = work / f"{mode}.tilt"
            program.write_text(f'''pipeline join:
  passos:
    - esquerda = ler_csv "esquerda.csv", colunar: verdadeiro, tipos: {{ id: "inteiro" }}
    - direita = ler_csv "direita_{direita}.csv", colunar: verdadeiro, tipos: {{ id: "inteiro" }}
    - resultado = esquerda.juntar direita, por: ["grupo", "id"]
    - imprimir tamanho(resultado)
''')
            command = [binary, "executar", program.name]
            for _ in range(args.repetitions):
                start = time.perf_counter_ns()
                result = subprocess.run(command, cwd=work, capture_output=True, text=True)
                elapsed = (time.perf_counter_ns() - start) / 1_000_000
                if result.returncode != 0:
                    raise RuntimeError(result.stderr)
                if result.stdout.strip().splitlines()[-1].strip() != str(args.rows):
                    raise RuntimeError("resultado do join composto incorreto")
                samples[mode].append(elapsed)
        for mode in ("merge", "hash"):
            print(f"{mode},rows={args.rows},median_ms={statistics.median(samples[mode]):.3f}")
        print(f"speedup_merge={statistics.median(samples['hash']) / statistics.median(samples['merge']):.3f}x")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
