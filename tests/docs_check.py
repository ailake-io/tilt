#!/usr/bin/env python3
"""Verifica os exemplos tilt embutidos na documentacao.

Uso: docs_check.py <bin-tilt> <doc1.md> [doc2.md ...]
Para cada bloco ```tilt <tag>:
  run   -> `tilt checar` + `tilt executar` precisam passar (hermetico:
           sem rede/servidores; cada bloco roda num diretorio temporario
           proprio, com TILT_LLM=mock e NO_COLOR=1).
  check -> so `tilt checar` (precisa de servicos externos: bancos, Kafka,
           S3, Livy, LLM real, ou `tilt servir`).
  skip  -> fragmento intencional (forma de valor, trecho parcial): ignorado.
Bloco sem tag falha de proposito (toda novidade precisa escolher a tag).

Saida 0 se tudo passa; imprime os blocos com falha e o motivo.
"""
import os
import re
import subprocess
import sys
import tempfile

BIN = sys.argv[1]
DOCS = sys.argv[2:]

PAT = re.compile(r"```tilt(\s+\w+)?\n(.*?)```", re.S)
TOTAL_FAILS = 0


def roda(doc, idx, tag, code):
    if tag == "skip":
        print(f"[{doc} bloco {idx}] SKIP (fragmento)")
        return False
    if tag not in ("run", "check"):
        print(f"[{doc} bloco {idx}] FALHA-tag: sem anotacao (use run|check|skip)")
        return True
    with tempfile.TemporaryDirectory() as tmpd:
        path = os.path.join(tmpd, "exemplo.tilt")
        with open(path, "w", encoding="utf-8") as tf:
            tf.write(code)
        env = {**os.environ, "TILT_LLM": "mock", "NO_COLOR": "1"}
        r = subprocess.run([BIN, "checar", path], capture_output=True, text=True,
                           timeout=30, cwd=tmpd)
        out = (r.stdout + r.stderr).strip().split("\n")
        if r.returncode != 0 or not any(l.startswith("ok:") for l in out):
            print(f"[{doc} bloco {idx}] FALHA-checar (tag={tag}):")
            print("  codigo:\n" + "\n".join("  | " + l for l in code.split("\n")[:12]))
            print("  erro:\n" + "\n".join("  | " + l for l in out[:8]))
            return True
        if tag == "run":
            r2 = subprocess.run([BIN, "executar", path], capture_output=True, text=True,
                                timeout=120, env=env, cwd=tmpd)
            if r2.returncode != 0:
                print(f"[{doc} bloco {idx}] FALHA-executar (tag={tag}):")
                print("  codigo:\n" + "\n".join("  | " + l for l in code.split("\n")[:12]))
                print("  erro:\n" + "\n".join(
                    "  | " + l for l in (r2.stdout + r2.stderr).strip().split("\n")[:8]))
                return True
    print(f"[{doc} bloco {idx}] OK (tag={tag})")
    return False


def main():
    fails = 0
    count = 0
    for doc in DOCS:
        with open(doc, encoding="utf-8") as f:
            src = f.read()
        for i, m in enumerate(PAT.finditer(src)):
            tag = (m.group(1) or "").strip()
            count += 1
            if roda(doc, i, tag, m.group(2)):
                fails += 1
    print(f"--- docs: {count - fails}/{count} blocos ok ---")
    return 1 if fails else 0


sys.exit(main())
