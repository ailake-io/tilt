#!/usr/bin/env sh
# Interop com a linguagem Kof nas duas direcoes, com o toolchain real:
#   Kof -> Tilt: `kof run` chama funcoes por `tilt rpc --porta` (kof.http + json.decode<T>)
#   Tilt -> Kof: `tilt executar` faz http_post_json num `kof serve`
# Pula (exit 0) sem kof, curl ou python3. Usa portas livres sorteadas.
set -eu

BIN="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
EX="$(cd "$2" && pwd)"
for c in kof curl python3; do
  command -v "$c" >/dev/null 2>&1 || { echo "pulado: sem $c"; exit 0; }
done
tmp=$(mktemp -d)
pids=""
trap 'for p in $pids; do kill "$p" 2>/dev/null || true; done; rm -rf "$tmp"' EXIT
fail=0
porta_livre() { python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])'; }
espera() { # espera <url>: ate 60 s
  n=0
  until curl -s -o /dev/null "$1"; do
    n=$((n + 1)); [ "$n" -lt 60 ] || { echo "FALHA: $1 nao subiu"; return 1; }
    sleep 1
  done
}

# --- Kof -> Tilt
p1=$(porta_livre)
"$BIN" rpc "$EX/vendas.tilt" --porta "$p1" >"$tmp/rpc.log" 2>&1 &
pids="$pids $!"
espera "http://127.0.0.1:$p1/saude" || exit 1
mkdir "$tmp/k1"
sed "s/8090/$p1/" "$EX/kof/cliente.kf" >"$tmp/k1/cliente.kf"
out=$(cd "$tmp/k1" && kof run cliente.kf 2>&1) || { echo "FALHA: kof run: $out"; fail=1; }
[ "$out" = "alto
2
sul 35" ] || { echo "FALHA: saida do cliente Kof: $out"; fail=1; }

# --- Tilt -> Kof
p2=$(porta_livre)
mkdir "$tmp/k2"
cp "$EX/kof/servico/servico.kf" "$tmp/k2/servico.kf"
(cd "$tmp/k2" && exec kof serve servico.kf --port "$p2" >"$tmp/kof.log" 2>&1) &
pids="$pids $!"
espera "http://127.0.0.1:$p2/" || exit 1
sed "s/8091/$p2/" "$EX/kof/chamar_kof.tilt" >"$tmp/chamar_kof.tilt"
out=$("$BIN" executar "$tmp/chamar_kof.tilt" 2>&1) || { echo "FALHA: tilt executar: $out"; fail=1; }
echo "$out" | grep -qF "bia 25" || { echo "FALHA: Tilt->Kof: $out"; fail=1; }

[ "$fail" = 0 ] && echo "kof interop ok"
exit "$fail"
