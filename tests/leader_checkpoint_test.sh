#!/usr/bin/env sh
# Fase 12-4: checkpoint distribuido (TILT_CHECKPOINT_DIR) + eleicao de lider
# (TILT_LEADER_LEASE) para `tilt executar --agendar` multi-replica.
#
# 1. checkpoint compartilhado: dois processos seguidos com o mesmo
#    TILT_CHECKPOINT_DIR — o segundo nao reprocessa (offset veio do dir).
# 2. lider: com TILT_LEADER_LEASE ocupado por outro dono (lease futuro), o
#    tick e pulado com log "sem lideranca".
# 3. backend nao suportado (s3://) falha com erro claro.
set -eu

BIN="$1"
FIXTURE_DIR="$2"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cp "$FIXTURE_DIR/dados.csv" "$FIXTURE_DIR/input.tilt" "$tmp/"
mkdir "$tmp/compartilhado"

run() { # $1 = MAX, resto = env extra
  max="$1"; shift
  ( cd "$tmp" && env "$@" TILT_AGORA=2026-01-05T02:50:00 TILT_AGENDAR_MAX="$max" \
    "$BIN" executar --agendar input.tilt )
}

out1=$(run 2 "TILT_CHECKPOINT_DIR=$tmp/compartilhado")
echo "$out1" | grep -q "lote: 2 1 2" || { echo "run1 sem lote [1,2]"; echo "$out1"; exit 1; }
[ -f "$tmp/compartilhado/dados.csv.tilt-offset" ] || {
  echo "checkpoint nao foi para o dir compartilhado"; ls "$tmp/compartilhado"; exit 1
}
[ ! -f "$tmp/dados.csv.tilt-offset" ] || {
  echo "offset vazou para o path local com TILT_CHECKPOINT_DIR"; exit 1
}

out2=$(run 4 "TILT_CHECKPOINT_DIR=$tmp/compartilhado")
echo "$out2" | grep -q "janela nao fechou" || {
  echo "run2 reprocessou com checkpoint compartilhado"; echo "$out2"; exit 1
}

# Lider ocupado: lease com dono alheio e expiry no futuro.
futuro=$(python3 -c "import time; print(int(time.time())+600)")
echo "outro-host:9999 $futuro" > "$tmp/lider.lock"
out3=$(run 1 "TILT_LEADER_LEASE=$tmp/lider.lock" "TILT_CHECKPOINT_DIR=$tmp/compartilhado") || true
echo "$out3" | grep -q "sem lideranca" || {
  echo "esperado 'sem lideranca' com lease ocupado"; echo "$out3"; exit 1
}

# Backend s3 tem cobertura dedicada (checkpoint_backend_test.sh, com mock):
# aqui so se verifica que a URI e aceita (sem mock o save cai para memoria,
# mas o lote executa).
out4=$(run 1 "TILT_CHECKPOINT_DIR=s3://bucket/prefixo" 2>&1) || {
  echo "s3 inesperadamente fatal"; echo "$out4"; exit 1
}
echo "$out4" | grep -q "lote:" || {
  echo "sem lote com backend s3"; echo "$out4"; exit 1
}

echo "leader_checkpoint: ok"
