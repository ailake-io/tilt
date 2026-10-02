#!/usr/bin/env python3
"""Gera bench/vendas.csv (1 milhao de linhas) para bench/dados.tilt."""
import os
import random
import sys

linhas = int(sys.argv[1]) if len(sys.argv) > 1 else 1_000_000
destino = os.path.join(os.path.dirname(os.path.abspath(__file__)), "vendas.csv")
random.seed(1)
regioes = ["sul", "norte", "leste", "oeste", "centro"]
with open(destino, "w", encoding="utf-8") as f:
    f.write("regiao,cliente,valor\n")
    for i in range(linhas):
        f.write("%s,c%d,%d\n" % (random.choice(regioes), i % 5000, random.randint(1, 500)))
print("gerado:", destino, linhas, "linhas")
