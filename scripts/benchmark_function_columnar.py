#!/usr/bin/env python3
"""Mede o custo de passar uma tabela colunar por uma funcao Tilt."""

import argparse
from pathlib import Path
import shutil
import statistics
import subprocess
import tempfile
import time


ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("binario", type=Path)
    parser.add_argument("--repetitions", type=int, default=5)
    args = parser.parse_args()
    source = ROOT / "bench" / "vendas.csv"
    if not source.exists():
        subprocess.run(["python3", str(ROOT / "bench" / "gerar_csv.py")], check=True)

    read = 'ler_csv "vendas.csv", selecionar: ["regiao", "valor"], colunar: verdadeiro'
    direct = f'''pipeline p:
  passos:
    - t = {read}
    - f = t.filtrar linha.valor >= 250
    - r = f.agrupar_por "regiao", {{ total: somar "valor", n: contar }}
    - imprimir r
'''
    through_function = f'''funcao agregar t:
  f = t.filtrar linha.valor >= 250
  retornar f.agrupar_por "regiao", {{ total: somar "valor", n: contar }}

pipeline p:
  passos:
    - t = {read}
    - imprimir agregar(t)
'''
    with tempfile.TemporaryDirectory(prefix="tilt-function-columnar-") as temp:
        work = Path(temp)
        shutil.copyfile(source, work / "vendas.csv")
        (work / "direto.tilt").write_text(direct)
        (work / "funcao.tilt").write_text(through_function)
        samples = {"direto": [], "funcao": []}
        outputs = {}
        for attempt in range(args.repetitions + 1):
            for name, file in (("direto", "direto.tilt"), ("funcao", "funcao.tilt")):
                start = time.perf_counter()
                result = subprocess.run(
                    [str(args.binario.resolve()), "executar", file], cwd=work,
                    text=True, capture_output=True, check=True,
                )
                elapsed = (time.perf_counter() - start) * 1000
                outputs[name] = result.stdout
                if attempt:
                    samples[name].append(elapsed)
        assert outputs["direto"] == outputs["funcao"]
        for name in samples:
            print(f"{name}: {statistics.median(samples[name]):.1f} ms")


if __name__ == "__main__":
    main()
