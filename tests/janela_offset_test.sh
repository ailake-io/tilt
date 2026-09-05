#!/usr/bin/env sh
# Offset persistente de janela de contagem em arquivo (`<fonte>.tilt-offset`):
# dois processos `tilt executar --agendar` seguidos no mesmo diretorio — o
# segundo le o offset gravado pelo primeiro e NAO reprocessa elementos ja
# consumidos. Tambem verifica que TILT_JANELA_ESTADO=memoria volta ao
# comportamento antigo (sem arquivo).
set -eu

BIN="$1"
FIXTURE_DIR="$2"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cp "$FIXTURE_DIR/dados.csv" "$FIXTURE_DIR/input.tilt" "$tmp/"

run() { # $1 = TILT_AGENDAR_MAX, $2 = estado (opcional; "memoria" desliga arquivo)
  (
    cd "$tmp"
    if [ -n "${2:-}" ]; then
      TILT_AGORA=2026-01-05T02:50:00 TILT_AGENDAR_MAX="$1" TILT_JANELA_ESTADO="$2" \
        "$BIN" executar --agendar input.tilt
    else
      TILT_AGORA=2026-01-05T02:50:00 TILT_AGENDAR_MAX="$1" \
        "$BIN" executar --agendar input.tilt
    fi
  )
}

out1=$(run 2)
echo "$out1" | grep -q "lote: 2 1 2" || {
  echo "run 1: esperado lote [1,2]"; echo "$out1"; exit 1
}
echo "$out1" | grep -q "lote: 2 3 4" || {
  echo "run 1: esperado lote [3,4]"; echo "$out1"; exit 1
}

[ -f "$tmp/dados.csv.tilt-offset" ] || {
  echo "arquivo de offset nao foi criado"; exit 1
}
grep -q '"contagens": 4' "$tmp/dados.csv.tilt-offset" || {
  echo "offset persistido errado:"; cat "$tmp/dados.csv.tilt-offset"; exit 1
}

# Segundo processo: fonte nao cresceu; nenhuma janela deve fechar de novo.
out2=$(run 4)
if echo "$out2" | grep -q "lote:"; then
  echo "run 2: reprocessou elementos ja consumidos"; echo "$out2"; exit 1
fi
echo "$out2" | grep -q "janela nao fechou" || {
  echo "run 2: esperado 'janela nao fechou'"; echo "$out2"; exit 1
}

# TILT_JANELA_ESTADO=memoria: rele do inicio e nao cria arquivo.
rm "$tmp/dados.csv.tilt-offset"
out3=$(run 2 memoria)
echo "$out3" | grep -q "lote: 2 1 2" || {
  echo "run 3 (memoria): esperado lote [1,2]"; echo "$out3"; exit 1
}
[ ! -f "$tmp/dados.csv.tilt-offset" ] || {
  echo "TILT_JANELA_ESTADO=memoria nao deve criar arquivo de offset"; exit 1
}

echo "janela_offset: ok"
