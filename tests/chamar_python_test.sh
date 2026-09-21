#!/usr/bin/env sh
# `chamar_python` (Tilt -> Python): modulo da stdlib, arquivo .py ao lado do
# programa, argumentos nomeados, tabela de ida e volta, erro do Python capturavel,
# stdout do modulo fora do stdout do Tilt e recusa de nomes com metacaracteres
# de shell. Pula (exit 0) se nao houver python3.
set -eu

BIN="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
PY="${TILT_PYTHON:-python3}"
command -v "$PY" >/dev/null 2>&1 || { echo "pulado: sem $PY"; exit 0; }
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fail=0

cat >"$tmp/modp.py" <<'PYEOF'
def normalizar(linhas, escala=1):
    print("ruido do python")
    return [{"regiao": l["regiao"].upper(), "valor": l["valor"] * escala} for l in linhas]

def falha():
    raise ValueError("deu ruim")
PYEOF
cat >"$tmp/p.tilt" <<'TILTEOF'
pipeline p:
  passos:
    - imprimir chamar_python("math", "sqrt", 16)
    - imprimir chamar_python("os.path", "join", "a", "b")
    - t = chamar_python "modp.py", "normalizar", [{ regiao: "sul", valor: 3 }], escala: 10
    - imprimir tipo_de(t), t[0].regiao, t[0].valor
    - tentar:
        chamar_python "modp.py", "falha"
    capturar erro:
      imprimir "erro: {{erro}}"
    - tentar:
        chamar_python "os; rm -rf x", "f"
    capturar erro:
      imprimir "erro: {{erro}}"
TILTEOF

# roda de outro diretorio: o .py e achado ao lado do programa
out=$(cd / && "$BIN" executar "$tmp/p.tilt" 2>"$tmp/err")
for esperado in "4" "a/b" "tabela SUL 30" "erro: python: ValueError: deu ruim (modp.py:6)" \
  "erro: chamar_python: modulo invalido 'os; rm -rf x'"; do
  echo "$out" | grep -qF -- "$esperado" || { echo "FALHA: faltou '$esperado' em: $out"; fail=1; }
done
echo "$out" | grep -q "ruido do python" && { echo "FALHA: stdout do modulo vazou para o stdout"; fail=1; }
grep -q "ruido do python" "$tmp/err" || { echo "FALHA: stdout do modulo devia ir para stderr"; fail=1; }

# executavel Python inexistente -> erro claro, nao trava
printf 'pipeline p:\n  passos:\n    - imprimir chamar_python("math", "sqrt", 4, python: "python-inexistente-xyz")\n' >"$tmp/q.tilt"
"$BIN" executar "$tmp/q.tilt" >/dev/null 2>"$tmp/err2" && { echo "FALHA: devia falhar"; fail=1; }
grep -q "chamar_python" "$tmp/err2" || { echo "FALHA: erro sem contexto: $(cat "$tmp/err2")"; fail=1; }

[ "$fail" = 0 ] && echo "chamar_python ok"
exit "$fail"
