#!/usr/bin/env sh
set -eu

BIN="$1"
TMP="$(mktemp -d)"
trap 'kill "$srv" 2>/dev/null || true; rm -rf "$TMP"' EXIT
PORT_FILE="$TMP/port"
python3 - "$PORT_FILE" <<'PY' >"$TMP/server.log" 2>&1 &
import http.server, json, sys
from http.server import ThreadingHTTPServer

port_file = sys.argv[1]
class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args): pass
    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0"))
        body = json.loads(self.rfile.read(n) or b"{}")
        if self.path.startswith("/keys/") and self.headers.get("Authorization") != "Bearer x":
            self.send_response(401)
            self.end_headers()
            return
        if ":encrypt" in self.path or ":decrypt" in self.path:
            if self.headers.get("Authorization") != "Bearer x":
                self.send_response(401)
                self.end_headers()
                return
        if "/transit/" in self.path and self.headers.get("X-Vault-Token") != "x":
            self.send_response(401)
            self.end_headers()
            return
        if self.path.endswith("/wrap?api-version=7.4") or self.path.endswith("/unwrap?api-version=7.4"):
            out = {"value": body.get("value", "")}
        elif self.path.endswith(":encrypt"):
            out = {"ciphertext": body.get("plaintext", "")}
        elif self.path.endswith(":decrypt"):
            out = {"plaintext": body.get("ciphertext", "")}
        elif "/transit/encrypt/" in self.path:
            out = {"data": {"ciphertext": body.get("plaintext", "")}}
        elif "/transit/decrypt/" in self.path:
            out = {"data": {"plaintext": body.get("ciphertext", "")}}
        else:
            self.send_response(404)
            self.end_headers()
            return
        raw = json.dumps(out).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)

srv = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
with open(port_file, "w") as f:
    f.write(str(srv.server_address[1]))
srv.serve_forever()
PY
srv=$!
for _ in $(seq 1 50); do [ -s "$PORT_FILE" ] && break; sleep .1; done
[ -s "$PORT_FILE" ]
PORT=$(cat "$PORT_FILE")
cat >"$TMP/cloud.tilt" <<EOF
pipeline cloud:
  passos:
    - dados = [{ id: 9 }]
    - escrever_parquet dados, "azure.parquet", chave_azure: "http://127.0.0.1:$PORT/keys/demo/1"
    - a = ler_parquet "azure.parquet"
    - escrever_parquet dados, "gcp.parquet", chave_gcp: "projects/p/locations/l/keyRings/r/cryptoKeys/k/cryptoKeyVersions/1"
    - g = ler_parquet "gcp.parquet"
    - escrever_parquet dados, "vault.parquet", chave_vault: "transit/demo"
    - v = ler_parquet "vault.parquet"
    - imprimir a[0].id, g[0].id, v[0].id
EOF
(cd "$TMP" && AZURE_KEY_VAULT_TOKEN=x GCP_ACCESS_TOKEN=x GCP_KMS_ENDPOINT="http://127.0.0.1:$PORT" \
  VAULT_ADDR="http://127.0.0.1:$PORT" VAULT_TOKEN=x "$BIN" executar cloud.tilt) >"$TMP/out"
grep -F '9 9 9' "$TMP/out" >/dev/null
echo "parquet_cloud_provider_test ok"
