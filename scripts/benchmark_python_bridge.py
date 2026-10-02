#!/usr/bin/env python3
"""Compara transporte Python↔Tilt JSON e Parquet com dados equivalentes.

Os objetos de entrada ficam prontos antes da medicao. O processo Tilt e
reutilizado; cada amostra inclui serializacao, RPC e desserializacao completa.
"""

import argparse
import os
import statistics
import sys
import tempfile
import time


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("binario")
    parser.add_argument("--rows", type=int, default=100_000)
    parser.add_argument("--repetitions", type=int, default=5)
    args = parser.parse_args()
    if args.rows <= 0 or args.repetitions <= 0:
        parser.error("rows e repetitions devem ser positivos")

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    sys.path.insert(0, os.path.join(root, "python"))
    os.environ["TILT_BIN"] = os.path.abspath(args.binario)
    import pyarrow as pa
    import tilt

    table = pa.table(
        {
            "id": list(range(args.rows)),
            "grupo": ["sul", "norte", "leste", "oeste"] * (args.rows // 4)
            + ["sul", "norte", "leste", "oeste"][: args.rows % 4],
            "valor": [i * 0.5 for i in range(args.rows)],
        }
    )
    rows = table.to_pylist()
    with tempfile.TemporaryDirectory(prefix="tilt-bench-python-") as directory:
        source = os.path.join(directory, "eco.tilt")
        with open(source, "w", encoding="utf-8") as output:
            output.write("funcao eco linhas:\n  retornar linhas\n")
        with tilt.carregar(source) as program:
            samples = {"json": [], "parquet": []}
            for index in range(args.repetitions + 1):
                for mode in ("json", "parquet"):
                    start = time.perf_counter()
                    if mode == "json":
                        result = program.chamar("eco", rows)
                        assert len(result) == args.rows and result[0] == rows[0]
                    else:
                        result = program.chamar_colunar("eco", table)
                        assert result.num_rows == args.rows and result.slice(0, 1).to_pylist()[0] == rows[0]
                    elapsed_ms = (time.perf_counter() - start) * 1000
                    if index > 0:
                        samples[mode].append(elapsed_ms)
            for mode in ("json", "parquet"):
                print(f"{mode}: {statistics.median(samples[mode]):.1f} ms")
            print(f"linhas: {args.rows}, repeticoes: {args.repetitions}")


if __name__ == "__main__":
    main()
