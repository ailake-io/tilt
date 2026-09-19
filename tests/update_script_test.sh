#!/usr/bin/env sh
# Exercita o atualizador contra um manifesto/servidor HTTP local: download,
# verificacao SHA-256, extracao e troca da instalacao.
set -eu

BIN="$1"
ROOT="$2"
PORTA=8493
command -v curl >/dev/null 2>&1 || { echo "curl ausente; pulando update_script"; exit 0; }
command -v python3 >/dev/null 2>&1 || { echo "python3 ausente; pulando update_script"; exit 0; }
command -v tar >/dev/null 2>&1 || { echo "tar ausente; pulando update_script"; exit 0; }

TMP=$(mktemp -d)
trap 'kill "$server" 2>/dev/null || true; rm -rf "$TMP"' EXIT
mkdir -p "$TMP/release/bin" "$TMP/release/share/tilt/stdlib" "$TMP/web"
cp "$BIN" "$TMP/release/bin/tilt"
cp -R "$ROOT/stdlib/." "$TMP/release/share/tilt/stdlib/"
ASSET="tilt-0.1.0-Linux-x86_64.tar.gz"
tar -czf "$TMP/web/$ASSET" -C "$TMP/release" bin share
( cd "$TMP/web" && sha256sum "$ASSET" > "$ASSET.sha256" )
python3 - "$TMP/web" "$PORTA" <<'PY' &
import http.server, os, sys
os.chdir(sys.argv[1])
server = http.server.ThreadingHTTPServer(("127.0.0.1", int(sys.argv[2])), http.server.SimpleHTTPRequestHandler)
server.serve_forever()
PY
server=$!
for _ in $(seq 1 30); do
  curl -fsS "http://127.0.0.1:$PORTA/$ASSET.sha256" >/dev/null 2>&1 && break
  sleep 0.1
done
cat >"$TMP/release.json" <<EOF
{"tag_name":"v0.1.0","assets":[
  {"name":"$ASSET","browser_download_url":"http://127.0.0.1:$PORTA/$ASSET"},
  {"name":"$ASSET.sha256","browser_download_url":"http://127.0.0.1:$PORTA/$ASSET.sha256"}
]}
EOF
PREFIX="$TMP/install"
sh "$ROOT/scripts/update.sh" --api-url="file://$TMP/release.json" --prefix="$PREFIX" --force
"$PREFIX/bin/tilt" versao | grep -q '^tilt 0.1.0$'
[ -f "$PREFIX/share/tilt/stdlib/io.tilt" ] || {
  echo "stdlib nao instalada pelo atualizador"; exit 1;
}
echo "update_script_test ok"
