#!/usr/bin/env sh
# Cursor incremental com watermark persistente e backfill inclusivo.
set -eu

BIN="$1"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

cat >"$tmp/dados.csv" <<'EOF'
id,nome
1,ana
2,bruno
3,carla
4,diego
EOF

cat >"$tmp/cursor.tilt" <<'EOF'
fonte eventos:
  tipo: csv
  caminho: "dados.csv"

pipeline incremental:
  agenda: "*/1 * * * *"
  entrada: eventos
  janela: 2
  desde: id
  passos:
    - imprimir "cursor:", tamanho(linhas), linhas[0].id, linhas[-1].id
EOF

out1=$(cd "$tmp" && TILT_AGORA=2026-01-05T02:50:00 TILT_AGENDAR_MAX=1 "$BIN" executar --agendar cursor.tilt)
echo "$out1" | grep -q "cursor: 2 1 2" || {
  echo "cursor: primeiro lote incorreto"; echo "$out1"; exit 1; }
grep -q '"cursor": 2' "$tmp/dados.csv.tilt-offset" || {
  echo "cursor: watermark nao persistido"; cat "$tmp/dados.csv.tilt-offset"; exit 1; }

out2=$(cd "$tmp" && TILT_AGORA=2026-01-05T02:50:00 TILT_AGENDAR_MAX=1 "$BIN" executar --agendar cursor.tilt)
echo "$out2" | grep -q "cursor: 2 3 4" || {
  echo "cursor: segundo lote reprocessado ou ausente"; echo "$out2"; exit 1; }
grep -q '"cursor": 4' "$tmp/dados.csv.tilt-offset" || {
  echo "cursor: segundo watermark incorreto"; cat "$tmp/dados.csv.tilt-offset"; exit 1; }

cat >"$tmp/backfill.tilt" <<'EOF'
fonte eventos:
  tipo: csv
  caminho: "dados.csv"

pipeline reprocessar:
  agenda: "*/1 * * * *"
  entrada: eventos
  janela: "1min"
  desde: id
  backfill: { desde: 2, ate: 3 }
  passos:
    - imprimir "backfill:", tamanho(linhas), linhas[0].id, linhas[-1].id
EOF

outb=$(cd "$tmp" && TILT_AGORA=2026-01-05T02:50:00 TILT_AGENDAR_MAX=1 TILT_JANELA_ESTADO=memoria "$BIN" executar --agendar backfill.tilt)
echo "$outb" | grep -q "backfill: 2 2 3" || {
  echo "backfill: intervalo incorreto"; echo "$outb"; exit 1; }

echo "cursor_backfill: ok"
