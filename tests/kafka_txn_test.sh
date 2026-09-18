#!/usr/bin/env sh
set -eu
BIN="$1"
FIXTURE="$2"
PORT="${TILT_TEST_PORT:-0}"
command -v python3 >/dev/null 2>&1 || { echo "python3 ausente; pulando o teste Kafka transacional"; exit 0; }
tmp=$(mktemp -d)
trap 'kill "$mock_pid" 2>/dev/null || true; rm -rf "$tmp"' EXIT
python3 - "$PORT" "$tmp/porta" "$tmp/log" <<'PY' >"$tmp/mock.out" 2>&1 &
import socketserver, struct, sys
port, port_file, log_path = int(sys.argv[1]), sys.argv[2], sys.argv[3]
store, pending, log = {}, {}, open(log_path, "w", encoding="utf-8")
def p8(v): return struct.pack(">b", v)
def p16(v): return struct.pack(">h", v)
def p32(v): return struct.pack(">i", v)
def p64(v): return struct.pack(">q", v)
def pstr(s):
    b=s.encode(); return p16(len(b))+b
def pbytes(b): return p32(len(b))+b
def rdstr(b,p):
    n=struct.unpack_from(">h",b,p)[0]; p+=2
    return ("",p) if n<0 else (b[p:p+n].decode(),p+n)
def rd32(b,p): return struct.unpack_from(">i",b,p)[0],p+4
def rd64(b,p): return struct.unpack_from(">q",b,p)[0],p+8
def svar(b,p):
    n=0; shift=0
    while True:
        c=b[p]; p+=1; n|=(c&127)<<shift
        if not c&128: return (n>>1)^-(n&1),p
        shift+=7
def meta():
    out=p32(1)+p32(0)+pstr("127.0.0.1")+p32(server.server_address[1])
    out+=p32(1)+pstr("vendas")+p32(2)
    for part in (0,1): out+=p16(0)+p32(part)+p32(0)+p32(1)+p32(0)+p32(1)+p32(0)
    log.write("METADATA\n"); log.flush(); return out
def coord(body):
    tx,_=rdstr(body,0); log.write("FINDCOORDINATOR "+tx+"\n"); log.flush()
    return p16(0)+p32(0)+pstr("127.0.0.1")+p32(server.server_address[1])
def init(body):
    tx,_=rdstr(body,0); log.write("INITPRODUCERID "+tx+"\n"); log.flush()
    return p32(0)+p16(0)+p64(123)+p16(0)
def add(body):
    tx,p=rdstr(body,0); _pid,p=rd64(body,p); p+=2; nt,p=rd32(body,p); topics=[]
    for _ in range(nt):
        t,p=rdstr(body,p); np,p=rd32(body,p); parts=[]
        for _ in range(np): q,p=rd32(body,p); parts.append(q)
        topics.append((t,parts))
    pending.setdefault(tx,[]); log.write("ADDPARTITIONS "+tx+" "+repr(topics)+"\n"); log.flush()
    out=p32(0)+p32(len(topics))
    for t,parts in topics:
        out+=pstr(t)+p32(len(parts))
        for q in parts: out+=p32(q)+p16(0)
    return out
def produce(body):
    tx,p=rdstr(body,0); p+=2+4; nt,p=rd32(body,p); got=[]
    for _ in range(nt):
        t,p=rdstr(body,p); np,p=rd32(body,p)
        for _ in range(np):
            q,p=rd32(body,p); size,p=rd32(body,p); batch=body[p:p+size]; p+=size
            rp=61; _n,rp=svar(batch,rp); _a,rp=svar(batch,rp); _ts,rp=svar(batch,rp); _od,rp=svar(batch,rp)
            kl,rp=svar(batch,rp)
            if kl>=0: rp+=kl
            vl,rp=svar(batch,rp); val=batch[rp:rp+vl].decode(); got.append((t,q,val))
    pending.setdefault(tx,[]).extend(got); log.write("PRODUCE "+tx+" "+repr(got)+"\n"); log.flush()
    out=p32(0)+p32(nt)
    for t,q,_ in got: out+=pstr(t)+p32(1)+p32(q)+p16(0)+p64(0)+p64(0)+p64(0)
    return out
def end(body):
    tx,p=rdstr(body,0); p+=8+2; commit=body[p]!=0; got=pending.pop(tx,[])
    if commit:
        for t,q,v in got: store.setdefault((t,q),[]).append(v)
    log.write("ENDTXN "+tx+" "+("COMMIT" if commit else "ABORT")+" "+str(len(got))+"\n"); log.flush(); return p32(0)+p16(0)
def message(v):
    body=p8(0)+p8(0)+p32(-1)+pbytes(v.encode())
    return p64(0)+p32(4+len(body))+p32(0)+body
def fetch(body):
    p=12; nt,p=rd32(body,p); t,p=rdstr(body,p); np,p=rd32(body,p); q,p=rd32(body,p); p+=8+4
    vals=store.get((t,q),[]); ms=b"".join(message(v) for v in vals)
    log.write("FETCH "+t+" "+str(q)+"\n"); log.flush()
    return p32(1)+pstr(t)+p32(1)+p32(q)+p16(0)+p64(len(vals))+pbytes(ms)
class H(socketserver.BaseRequestHandler):
    def handle(self):
        h=self.request.recv(4)
        if len(h)!=4:return
        n=struct.unpack(">i",h)[0]; data=b""
        while len(data)<n: data+=self.request.recv(n-len(data))
        api,ver,corr=struct.unpack_from(">hhi",data,0); p=8; _,p=rdstr(data,p); body=data[p:]
        if api==3: out=meta()
        elif api==10: out=coord(body)
        elif api==22: out=init(body)
        elif api==24: out=add(body)
        elif api==0 and ver>=3: out=produce(body)
        elif api==26: out=end(body)
        elif api==1: out=fetch(body)
        else: raise RuntimeError("API inesperada %s v%s"%(api,ver))
        frame=p32(corr)+out; self.request.sendall(p32(len(frame))+frame)
server=socketserver.ThreadingTCPServer(("127.0.0.1",port),H)
with open(port_file,"w") as f:f.write(str(server.server_address[1]))
server.serve_forever()
PY
mock_pid=$!
for _ in $(seq 1 50); do test -s "$tmp/porta" && break; sleep 0.1; done
test -s "$tmp/porta"
KAFKA_BOOTSTRAP="127.0.0.1:$(cat "$tmp/porta")" "$BIN" executar "$FIXTURE" >"$tmp/out"
grep -Fx '1' "$tmp/out"
grep -Fx 'v-0' "$tmp/out"
grep -Fx 'FINDCOORDINATOR tilt-tx-1' "$tmp/log"
grep -Fx 'INITPRODUCERID tilt-tx-1' "$tmp/log"
grep -F 'ADDPARTITIONS tilt-tx-1' "$tmp/log"
test "$(grep -c '^PRODUCE tilt-tx-1' "$tmp/log")" -eq 2
grep -Fx 'ENDTXN tilt-tx-1 COMMIT 2' "$tmp/log"
echo "kafka transacional: ok"
