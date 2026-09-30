#!/usr/bin/env python3
"""Mede derivacao, ordenacao e agregacao colunar em cargas grandes."""

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
    parser.add_argument("--rows", type=int, default=2_000_000)
    parser.add_argument("--repetitions", type=int, default=3)
    args = parser.parse_args()
    if args.rows < 1 or args.repetitions < 1:
        parser.error("rows e repetitions devem ser positivos")

    with tempfile.TemporaryDirectory(prefix="tilt-columnar-large-") as tmp:
        work = Path(tmp)
        with (work / "vendas.csv").open("w", newline="") as handle:
            writer = csv.writer(handle)
            writer.writerow(("id", "regiao", "valor"))
            regioes = ("norte", "sul", "leste", "oeste", "centro")
            for row in range(args.rows):
                writer.writerow((row, regioes[row % len(regioes)], (row * 17) % 500))

        programs = {
            "derivar": '''pipeline p:
  passos:
    - t = ler_csv "vendas.csv", colunar: verdadeiro
    - d = t.derivar { ajustado: linha.valor * 2 }
    - imprimir tamanho(d)
''',
            "ordenar": '''pipeline p:
  passos:
    - t = ler_csv "vendas.csv", colunar: verdadeiro
    - d = t.ordenar_por "valor", desc: verdadeiro
    - imprimir tamanho(d)
''',
            "agrupar": '''pipeline p:
  passos:
    - t = ler_csv "vendas.csv", colunar: verdadeiro
    - d = t.agrupar_por "regiao", { total: somar "valor", n: contar }
    - imprimir tamanho(d)
''',
        }
        samples: dict[str, list[float]] = {name: [] for name in programs}
        binary = str(args.tilt.resolve())
        for name, source in programs.items():
            path = work / f"{name}.tilt"
            path.write_text(source, encoding="utf-8")
            for _ in range(args.repetitions):
                started = time.perf_counter_ns()
                result = subprocess.run([binary, "executar", path.name], cwd=work,
                                        capture_output=True, text=True, check=True)
                elapsed = (time.perf_counter_ns() - started) / 1_000_000
                expected = "5" if name == "agrupar" else str(args.rows)
                if expected not in result.stdout:
                    raise RuntimeError(f"resultado inesperado em {name}: {result.stdout}")
                samples[name].append(elapsed)
        for name, values in samples.items():
            print(f"{name},rows={args.rows},median_ms={statistics.median(values):.3f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
