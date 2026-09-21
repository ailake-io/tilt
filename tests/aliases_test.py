#!/usr/bin/env python3
"""Palavras reservadas em ingles: cada NN.en.tilt de tests/aliases tem um gemeo
NN.pt.tilt (extraido da documentacao). Depois do lexer os dois devem ter o mesmo
mesmos tokens: as palavras-chave/funcoes embutidas em ingles viram as em portugues
e os nomes do usuario (iguais nos dois) e o tipo dos textos batem. Cada .en.tilt tambem precisa passar em `tilt checar`.

uso: aliases_test.py <tilt> <diretorio-tests/aliases>
"""
import os
import re
import subprocess
import sys
from pathlib import Path

tilt, pasta = sys.argv[1], Path(sys.argv[2])




def esqueleto(arquivo):
    saida = subprocess.run([tilt, "tokens", str(arquivo)], capture_output=True, text=True)
    if saida.returncode != 0:
        return None, saida.stderr
    toks = []
    for linha in saida.stdout.splitlines():
        m = re.match(r"\s*\d+:\d+\s+(\w+)(?:\s+'(.*)')?$", linha)
        if not m:
            continue
        tipo, lex = m.group(1), m.group(2)
        # Textos e numeros: so o tipo (as frases mudam entre os gemeos).
        toks.append(lex if tipo == "IDENT" else tipo)
    return toks, ""


falhas = 0
pares = sorted(pasta.glob("*.en.tilt"))
if not pares:
    print("nenhum par encontrado")
    sys.exit(1)
for en in pares:
    pt = en.with_name(en.name.replace(".en.tilt", ".pt.tilt"))
    a, erro_a = esqueleto(en)
    b, erro_b = esqueleto(pt)
    if a is None or b is None:
        print(f"FALHA {en.name}: tokens: {erro_a or erro_b}")
        falhas += 1
        continue
    if a != b:
        # Nomes que o runtime resolve (`r.text` -> `texto`, parametro `text`) nao mudam
        # no lexer: aceita se os dois programas produzem a mesma saida.
        env = {**os.environ, "TILT_LLM": "mock"}
        ra = subprocess.run([tilt, "executar", str(en)], capture_output=True, text=True, env=env)
        rb = subprocess.run([tilt, "executar", str(pt)], capture_output=True, text=True, env=env)
        if ra.returncode != 0 or ra.stdout != rb.stdout or ra.returncode != rb.returncode:
            falhas += 1
            i = next((k for k in range(min(len(a), len(b))) if a[k] != b[k]), min(len(a), len(b)))
            print(f"FALHA {en.name}: tokens diferem no {i}: en={a[max(i-2,0):i+3]} "
                  f"pt={b[max(i-2,0):i+3]} e a saida tambem:\n--- en\n{ra.stdout}{ra.stderr}"
                  f"--- pt\n{rb.stdout}{rb.stderr}")
            continue
    chk = subprocess.run([tilt, "checar", str(en)], capture_output=True, text=True)
    if chk.returncode != 0:
        falhas += 1
        print(f"FALHA {en.name}: checar:\n{chk.stderr or chk.stdout}")

if falhas == 0:
    print(f"aliases ok ({len(pares)} pares)")
sys.exit(1 if falhas else 0)
