#!/usr/bin/env python3
"""Mede materialização de mapas/listas do runtime com pool de Value."""

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
    with tempfile.TemporaryDirectory(prefix="tilt-value-pool-") as tmp:
        work = Path(tmp)
        with (work / "dados.csv").open("w", newline="") as handle:
            writer = csv.writer(handle)
            writer.writerow(("id", "valor", "rotulo"))
            for row in range(args.rows):
                writer.writerow((row, row * 0.5, f"r{row % 31}"))
        samples: dict[str, list[float]] = {"materializacao": [], "derivacao": []}
        programs = {
            "materializacao": '''pipeline p:
  passos:
    - t = ler_csv "dados.csv", colunar: verdadeiro
    - s = t.selecionar "id", "valor", "rotulo"
    - imprimir tamanho(s)
''',
            "derivacao": '''pipeline p:
  passos:
    - t = ler_csv "dados.csv", colunar: verdadeiro
    - s = t.derivar { dobro: linha.valor * 2 }
    - imprimir tamanho(s)
''',
        }
        for name, program in programs.items():
            (work / f"{name}.tilt").write_text(program)
            for _ in range(args.repetitions):
                start = time.perf_counter_ns()
                result = subprocess.run(
                    [str(args.tilt.resolve()), "executar", f"{name}.tilt"],
                    cwd=work, capture_output=True, text=True, check=True)
                samples[name].append((time.perf_counter_ns() - start) / 1_000_000)
                if result.stdout.strip().splitlines()[-1] != str(args.rows):
                    raise RuntimeError(f"{name}: contagem incorreta")
        print("operacao,rows,median_ms")
        for name, values in samples.items():
            print(f"{name},{args.rows},{statistics.median(values):.3f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
