#!/usr/bin/env sh
# Integration test for the Redis connector (`ler_redis`/`escrever_redis` with
# AUTH + SELECT; `redis_executar`/`redis_lote`): spins up a mock Redis server
# in python3 (pure socketserver, speaking RESP over TCP: AUTH with password
# "segredo", SELECT with per-db dict isolation, GET/SET/INCR/DEL/RPUSH/LRANGE/
# KEYS) and runs `tilt executar` on fixtures/redis_roundtrip.tilt and
# fixtures/redis_exec_lote.tilt, checking the printed values (roundtrip on
# db 1, empty read of the same key on db 0, captured AUTH failure with wrong
# password; executar com nil/integer/array/erro, lote numa unica conexao com
# respostas na ordem) and the mock's request log (AUTH/SELECT/command counts
# per db, total de conexoes abertas).
set -eu

BIN="$1"
PORT_BASE="${TILT_TEST_PORT:-8661}"

command -v python3 >/dev/null 2>&1 || {
  echo "python3 ausente; pulando o teste Redis"
  exit 0
}

tmp=$(mktemp -d)
trap 'kill "$mock_pid" 2>/dev/null || true; rm -rf "$tmp"' EXIT

# --- mock RESP ----------------------------------------------------------------
python3 - "$PORT_BASE" "$tmp/porta" "$tmp/log" <<'PYEOF' >"$tmp/mock_out" 2>&1 &
import socketserver
import sys

port_base = int(sys.argv[1])
port_file = sys.argv[2]
log_path = sys.argv[3]

log = open(log_path, "a", encoding="utf-8")

store = {}   # db -> {chave: valor}
listas = {}  # (db, chave) -> [valor, ...]
SENHA = "segredo"


class RespReader:
    def __init__(self, conn):
        self.conn = conn
        self.buf = b""

    def read_line(self):
        while b"\r\n" not in self.buf:
            chunk = self.conn.recv(4096)
            if not chunk:
                return None
            self.buf += chunk
        line, self.buf = self.buf.split(b"\r\n", 1)
        return line

    def read_n(self, n):
        while len(self.buf) < n:
            chunk = self.conn.recv(4096)
            if not chunk:
                return None
            self.buf += chunk
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def read_command(self):
        line = self.read_line()
        if line is None or not line.startswith(b"*"):
            return None
        nargs = int(line[1:])
        args = []
        for _ in range(nargs):
            hdr = self.read_line()
            if hdr is None or not hdr.startswith(b"$"):
                return None
            n = int(hdr[1:])
            data = self.read_n(n)
            if data is None or self.read_n(2) != b"\r\n":
                return None
            args.append(data.decode())
        return args


class RedisMock(socketserver.BaseRequestHandler):
    def handle(self):
        conn = self.request
        reader = RespReader(conn)
        log.write("CONN\n")
        log.flush()
        db = 0
        while True:
            args = reader.read_command()
            if args is None:
                return
            cmd = args[0].upper()
            if cmd == "AUTH":
                ok = len(args) >= 2 and args[-1] == SENHA
                log.write("AUTH %s\n" % ("ok" if ok else "falha"))
                log.flush()
                conn.sendall(b"+OK\r\n" if ok else b"-ERR invalid password\r\n")
            elif cmd == "SELECT":
                n = int(args[1])
                if 0 <= n <= 15:
                    db = n
                    log.write("SELECT %d\n" % n)
                    log.flush()
                    conn.sendall(b"+OK\r\n")
                else:
                    conn.sendall(b"-ERR DB index is out of range\r\n")
            elif cmd == "GET":
                chave = args[1]
                log.write("GET %d %s\n" % (db, chave))
                log.flush()
                valor = store.get(db, {}).get(chave)
                if valor is None:
                    conn.sendall(b"$-1\r\n")
                else:
                    b = valor.encode()
                    conn.sendall(b"$%d\r\n" % len(b) + b + b"\r\n")
            elif cmd == "SET":
                store.setdefault(db, {})[args[1]] = args[2]
                log.write("SET %d %s\n" % (db, args[1]))
                log.flush()
                conn.sendall(b"+OK\r\n")
            elif cmd == "INCR":
                chave = args[1]
                log.write("INCR %d %s\n" % (db, chave))
                log.flush()
                cur = store.get(db, {}).get(chave)
                n = (int(cur) if cur is not None and cur.lstrip("-").isdigit() else 0) + 1
                store.setdefault(db, {})[chave] = str(n)
                conn.sendall(b":%d\r\n" % n)
            elif cmd == "DEL":
                n = 0
                for chave in args[1:]:
                    if chave in store.get(db, {}):
                        del store[db][chave]
                        n += 1
                log.write("DEL %d %d\n" % (db, n))
                log.flush()
                conn.sendall(b":%d\r\n" % n)
            elif cmd == "RPUSH":
                chave = args[1]
                lst = listas.setdefault((db, chave), [])
                lst.extend(args[2:])
                log.write("RPUSH %d %s\n" % (db, chave))
                log.flush()
                conn.sendall(b":%d\r\n" % len(lst))
            elif cmd == "LRANGE":
                chave = args[1]
                ini, fim = int(args[2]), int(args[3])
                lst = listas.get((db, chave), [])
                if fim < 0:
                    fim = len(lst) + fim
                vals = lst[max(ini, 0):fim + 1]
                log.write("LRANGE %d %s\n" % (db, chave))
                log.flush()
                out = b"*%d\r\n" % len(vals)
                for v in vals:
                    b = v.encode()
                    out += b"$%d\r\n" % len(b) + b + b"\r\n"
                conn.sendall(out)
            elif cmd == "KEYS":
                chaves = sorted(k for k in store.get(db, {})
                                if args[1] == "*" or args[1] in k)
                log.write("KEYS %d\n" % db)
                log.flush()
                out = b"*%d\r\n" % len(chaves)
                for k in chaves:
                    b = k.encode()
                    out += b"$%d\r\n" % len(b) + b + b"\r\n"
                conn.sendall(out)
            else:
                conn.sendall(b"-ERR unknown command\r\n")


class Servidor(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


srv = None
for porta in range(port_base, port_base + 20):
    try:
        srv = Servidor(("127.0.0.1", porta), RedisMock)
        break
    except OSError:
        continue
if srv is None:
    sys.exit("nenhuma porta livre a partir de %d" % port_base)

with open(port_file, "w", encoding="utf-8") as f:
    f.write(str(porta))
srv.serve_forever()
PYEOF
mock_pid=$!

for _ in $(seq 1 50); do
  [ -s "$tmp/porta" ] && break
  sleep 0.1
done
[ -s "$tmp/porta" ] || { echo "mock Redis nao iniciou"; cat "$tmp/mock_out"; exit 1; }
PORTA=$(cat "$tmp/porta")

# --- roundtrip via tilt executar -------------------------------------------------
out=$(
  env REDIS_URL="redis://:segredo@127.0.0.1:$PORTA/0" \
    "$BIN" executar "${2:-${0%/*}/fixtures/redis_roundtrip.tilt}"
)

# congela o log da 1a fase: a 2a fase (db 0) tambem grava SET/GET no db 0
cp "$tmp/log" "$tmp/log1"

# --- redis_executar / redis_lote (mesmo mock, db 0 da URL) ------------------------
out2=$(
  env REDIS_URL="redis://:segredo@127.0.0.1:$PORTA/0" \
    "$BIN" executar "${0%/*}/fixtures/redis_exec_lote.tilt"
)

kill "$mock_pid" 2>/dev/null || true
mock_pid=""

fail=0
# (a) roundtrip no db 1: SET/GET com {banco: 1} devolve o valor gravado
echo "$out" | grep -q "valor-a" || { echo "saida sem 'valor-a': $out"; fail=1; }
# (b) a mesma chave no db 0 (URL, sem opcao 'banco') nao existe: erro capturado
echo "$out" | grep -q "chave 'chave-x' nao encontrada" || {
  echo "saida sem erro de chave ausente no db 0: $out"; fail=1;
}
# (c) senha errada em chamada separada: AUTH falha, erro capturado e impresso
echo "$out" | grep -q "redis: auth falhou (senha invalida?)" || {
  echo "saida sem 'auth falhou': $out"; fail=1;
}
# o ramo inesperado do tentar/capturar nao deve ter rodado
echo "$out" | grep -q "inesperado" && { echo "db 0 deveria estar vazio: $out"; fail=1; }

# (d) log do mock: AUTH/SELECT por conexao e isolamento por db
check_log() {
  esperado="$1"; shift
  n=$(grep -c "^$esperado\$" "$tmp/log1" || true)
  [ "$n" = "$1" ] || { echo "esperado $1 x '$esperado', obtido $n"; cat "$tmp/log1"; fail=1; }
}
check_log "AUTH ok" 3
check_log "AUTH falha" 1
check_log "SELECT 1" 2
check_log "SELECT 0" 1
check_log "SET 1 chave-x" 1
check_log "GET 1 chave-x" 1
check_log "GET 0 chave-x" 1
# SET no db 1 nunca deve aparecer no db 0
grep -q "^SET 0 " "$tmp/log1" && { echo "SET no db 0 inesperado"; cat "$tmp/log1"; fail=1; }

# (e) redis_executar / redis_lote: saida completa, na ordem
esperado2=$(cat <<'EOF'
== pipeline redis_exec ==
nil: nulo
set: OK
get: valor-exec
incr1: 1
incr2: 2
keys: [contador, k-exec]
erro: redis: ERR unknown command
lote: [1, 2, [a, b], 3, 1]
pos-del: nulo
vazio: redis: comando redis nao aceita argumento vazio
tipo: redis: o lote deve ser uma lista de listas [[comando, args...], ...]
lote-vazio: redis: lote de comandos nao pode ser vazio
EOF
)
[ "$out2" = "$esperado2" ] || { echo "saida de redis_exec_lote diferente:"; echo "$out2"; fail=1; }

# (f) log do mock: comandos do db 0 e contagem de conexoes
check_log() {
  esperado="$1"; shift
  n=$(grep -c "^$esperado\$" "$tmp/log" || true)
  [ "$n" = "$1" ] || { echo "esperado $1 x '$esperado', obtido $n"; cat "$tmp/log"; fail=1; }
}
check_log "GET 0 chave-inexistente" 1
check_log "SET 0 k-exec" 1
check_log "GET 0 k-exec" 2  # GET do roundtrip + GET pos-del
check_log "INCR 0 contador" 3  # 2 x executar + 1 no lote
check_log "KEYS 0" 1
check_log "RPUSH 0 fila" 2
check_log "LRANGE 0 fila" 1
check_log "DEL 0 1" 1
# conexoes: 4 (roundtrip) + 9 (executar/lote: 7 executar + 1 lote + 1 pos-del)
check_log "CONN" 13

[ "$fail" = 0 ] && echo "redis_test ok"
exit "$fail"
