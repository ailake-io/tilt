#!/usr/bin/env sh
# Cache .tiltc: miss+save na 1a vez, hit na 2a (saida identica), miss apos
# editar o fonte, fallback com arquivo corrompido e bypass via TILT_VM_NOCACHE.
set -eu

BIN="$1"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

printf 'funcao dobra v:\n  retornar v * 2\n\npipeline demo:\n  passos:\n    - imprimir dobra(21)\n' >"$tmp/a.tilt"

fail=0
# 1) Miss + save.
out1=$(TILT_VM_DEBUG=1 "$BIN" executar --vm "$tmp/a.tilt" 2>"$tmp/e1.log")
echo "$out1" | grep -q "42" || { echo "saida errada na 1a: $out1"; fail=1; }
grep -q "tiltc miss" "$tmp/e1.log" || { echo "sem miss na 1a"; fail=1; }
[ -f "$tmp/a.tiltc" ] || { echo "sem arquivo .tiltc"; fail=1; }

# 2) Hit com saida identica.
out2=$(TILT_VM_DEBUG=1 "$BIN" executar --vm "$tmp/a.tilt" 2>"$tmp/e2.log")
[ "$out1" = "$out2" ] || { echo "saida divergiu no hit"; fail=1; }
grep -q "tiltc hit" "$tmp/e2.log" || { echo "sem hit na 2a"; fail=1; }

# 3) Fonte editada invalida (miss) e roda a semantica nova.
printf 'funcao dobra v:\n  retornar v * 3\n\npipeline demo:\n  passos:\n    - imprimir dobra(21)\n' >"$tmp/a.tilt"
out3=$(TILT_VM_DEBUG=1 "$BIN" executar --vm "$tmp/a.tilt" 2>"$tmp/e3.log")
echo "$out3" | grep -q "63" || { echo "semantica nova nao rodou: $out3"; fail=1; }
grep -q "tiltc miss" "$tmp/e3.log" || { echo "edicao nao invalidou"; fail=1; }

# 4) Corrompido: roda igual (fallback) e regrava.
printf 'LIXO' | dd of="$tmp/a.tiltc" conv=notrunc bs=1 count=4 2>/dev/null
out4=$(TILT_VM_DEBUG=1 "$BIN" executar --vm "$tmp/a.tilt" 2>"$tmp/e4.log")
echo "$out4" | grep -q "63" || { echo "corrompido nao rodou: $out4"; fail=1; }
grep -q "tiltc miss" "$tmp/e4.log" || { echo "corrompido nao deu miss"; fail=1; }

# 5) Bypass: nao cria arquivo.
rm -f "$tmp/a.tiltc"
TILT_VM_NOCACHE=1 "$BIN" executar --vm "$tmp/a.tilt" >/dev/null 2>&1
[ ! -f "$tmp/a.tiltc" ] || { echo "bypass criou arquivo"; fail=1; }

[ "$fail" = 0 ] && echo "tiltc_test ok"
exit "$fail"
