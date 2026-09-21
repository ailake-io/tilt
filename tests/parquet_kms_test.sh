#!/usr/bin/env sh
set -eu

BIN="$1"
command -v python3 >/dev/null 2>&1 || {
  echo "python3 ausente; pulando o teste parquet_kms"
  exit 0
}
command -v curl >/dev/null 2>&1 || {
  echo "curl ausente; pulando o teste parquet_kms"
  exit 0
}

tmp=$(mktemp -d)
mock_pid=
trap 'if [ -n "$mock_pid" ]; then kill "$mock_pid" 2>/dev/null || true; fi; rm -rf "$tmp"' EXIT

python3 - "$tmp/port" "$tmp/requests.jsonl" <<'PYEOF' >"$tmp/mock.log" 2>&1 &
import base64
import hashlib
import hmac
import http.server
import json
import socketserver
import sys

port_path, log_path = sys.argv[1:]
access_key = "kms-test-access"
secret_key = "kms-test-secret-0123456789abcdef"
region = "us-east-1"
plain_key = b"K" * 32
ciphertext_blob = base64.b64encode(b"mock-kms-encrypted-data-key").decode()
resolved_key_id = "arn:aws:kms:us-east-1:111122223333:key/mock-key-id"
calls = []


def signed_request_ok(handler, payload):
    auth = handler.headers.get("Authorization", "")
    if not auth.startswith("AWS4-HMAC-SHA256 "):
        return False
    parts = {}
    for item in auth[len("AWS4-HMAC-SHA256 "):].split(","):
        key, _, value = item.strip().partition("=")
        parts[key] = value
    credential = parts.get("Credential", "").split("/", 1)
    if len(credential) != 2 or credential[0] != access_key:
        return False
    date_stamp, signed_region, service, terminal = credential[1].split("/")
    if (signed_region, service, terminal) != (region, "kms", "aws4_request"):
        return False
    signed_headers = parts.get("SignedHeaders", "").split(";")
    payload_hash = handler.headers.get("X-Amz-Content-Sha256", "")
    if hashlib.sha256(payload).hexdigest() != payload_hash:
        return False
    canonical_headers = ""
    for name in signed_headers:
        value = handler.headers.get(name)
        if name == "host":
            value = handler.headers.get("Host")
        if value is None:
            return False
        canonical_headers += name + ":" + value.strip() + "\n"
    canonical = (handler.command + "\n/\n\n" + canonical_headers + "\n" +
                 ";".join(signed_headers) + "\n" + payload_hash)
    amz_date = handler.headers.get("X-Amz-Date", "")
    scope = credential[1]
    string_to_sign = ("AWS4-HMAC-SHA256\n" + amz_date + "\n" + scope + "\n" +
                      hashlib.sha256(canonical.encode()).hexdigest())
    key = hmac.new(("AWS4" + secret_key).encode(), date_stamp.encode(), hashlib.sha256).digest()
    key = hmac.new(key, signed_region.encode(), hashlib.sha256).digest()
    key = hmac.new(key, service.encode(), hashlib.sha256).digest()
    key = hmac.new(key, terminal.encode(), hashlib.sha256).digest()
    expected = hmac.new(key, string_to_sign.encode(), hashlib.sha256).hexdigest()
    return hmac.compare_digest(expected, parts.get("Signature", ""))


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_args):
        pass

    def respond(self, status, value):
        body = json.dumps(value, separators=(",", ":")).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/x-amz-json-1.1")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        payload = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        if self.path != "/" or not signed_request_ok(self, payload):
            self.respond(403, {"message": "invalid SigV4 request"})
            return
        target = self.headers.get("X-Amz-Target", "")
        request = json.loads(payload)
        if target == "TrentService.GenerateDataKey":
            if request != {"KeyId": "alias/tilt-test", "KeySpec": "AES_256"}:
                self.respond(400, {"message": "unexpected GenerateDataKey request"})
                return
            calls.append({"target": target, "request": request})
            self.respond(200, {
                "Plaintext": base64.b64encode(plain_key).decode(),
                "CiphertextBlob": ciphertext_blob,
                "KeyId": resolved_key_id,
            })
        elif target == "TrentService.Decrypt":
            if request != {"KeyId": resolved_key_id, "CiphertextBlob": ciphertext_blob}:
                self.respond(400, {"message": "unexpected Decrypt request"})
                return
            calls.append({"target": target, "request": request})
            self.respond(200, {"Plaintext": base64.b64encode(plain_key).decode()})
        else:
            self.respond(400, {"message": "unexpected x-amz-target"})
            return
        with open(log_path, "w", encoding="utf-8") as log:
            for call in calls:
                log.write(json.dumps(call) + "\n")


class Server(socketserver.TCPServer):
    allow_reuse_address = True


with Server(("127.0.0.1", 0), Handler) as server:
    with open(port_path, "w", encoding="ascii") as port_file:
        port_file.write(str(server.server_address[1]))
    server.serve_forever()
PYEOF
mock_pid=$!

tries=0
while [ ! -s "$tmp/port" ]; do
  tries=$((tries + 1))
  if [ "$tries" -gt 100 ]; then
    echo "mock KMS nao iniciou"
    exit 1
  fi
  sleep 0.1
done
port=$(sed -n '1p' "$tmp/port")

python3 - "$tmp" <<'PYEOF'
import json
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
quoted_path = json.dumps("dados-kms.parquet")
(root / "gravar.tilt").write_text(
    "pipeline gravar:\n"
    "  passos:\n"
    "    - linhas = [{ id: 7, nome: \"kms\" }]\n"
    f"    - escrever_parquet linhas, {quoted_path}, chave_kms: \"alias/tilt-test\"\n",
    encoding="utf-8",
)
(root / "ler.tilt").write_text(
    "pipeline ler:\n"
    "  passos:\n"
    f"    - linhas = ler_parquet {quoted_path}\n"
    "    - imprimir tamanho linhas\n"
    "    - imprimir linhas[0].id, linhas[0].nome\n",
    encoding="utf-8",
)
PYEOF

export AWS_ACCESS_KEY_ID=kms-test-access
export AWS_SECRET_ACCESS_KEY=kms-test-secret-0123456789abcdef
export AWS_REGION=us-east-1
export KMS_ENDPOINT="http://127.0.0.1:$port"

out=$(cd "$tmp" && "$BIN" executar gravar.tilt)
printf '%s\n' "$out"
out=$(cd "$tmp" && "$BIN" executar ler.tilt)
printf '%s\n' "$out"
printf '%s\n' "$out" | grep -F "1" >/dev/null
printf '%s\n' "$out" | grep -F "7 kms" >/dev/null

python3 - "$tmp/dados-kms.parquet" "$tmp/requests.jsonl" <<'PYEOF'
import base64
import json
import pathlib
import sys

parquet = pathlib.Path(sys.argv[1]).read_bytes()
assert parquet[:4] == b"PARE" and parquet[-4:] == b"PARE"
assert b"aws-kms-v1" in parquet and b"ciphertext_blob" in parquet
assert base64.b64encode(b"mock-kms-encrypted-data-key") in parquet
assert b"K" * 32 not in parquet, "plaintext data key persisted in Parquet"
calls = [json.loads(line) for line in pathlib.Path(sys.argv[2]).read_text().splitlines()]
assert [call["target"] for call in calls] == [
    "TrentService.GenerateDataKey", "TrentService.Decrypt"
], calls
print("KMS mock: SigV4, GenerateDataKey, Decrypt e metadata sem plaintext ok")
PYEOF
