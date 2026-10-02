#!/usr/bin/env sh
set -eu

BIN="$1"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
cat >"$TMP/create.tilt" <<'EOF'
pipeline criar:
  passos:
    - dados = [{ id: 1 }]
    - escrever_delta dados, "delta"
    - escrever_iceberg dados, "iceberg"
EOF
cat >"$TMP/append_delta.tilt" <<'EOF'
pipeline anexar:
  passos:
    - dados = [{ id: 2 }]
    - anexar_delta dados, "delta"
EOF
cat >"$TMP/append_iceberg.tilt" <<'EOF'
pipeline anexar:
  passos:
    - dados = [{ id: 2 }]
    - anexar_iceberg dados, "iceberg"
EOF
(cd "$TMP" && "$BIN" executar create.tilt >/dev/null)
mkdir "$TMP/delta/_delta_log/.tilt.lock.d"
mkdir "$TMP/iceberg/.tilt.lock.d"
set +e
(cd "$TMP" && TILT_TABLE_LOCK_TIMEOUT_MS=0 "$BIN" executar append_delta.tilt) >"$TMP/delta.out" 2>&1
rc_delta=$?
(cd "$TMP" && TILT_TABLE_LOCK_TIMEOUT_MS=0 "$BIN" executar append_iceberg.tilt) >"$TMP/ice.out" 2>&1
rc_ice=$?
set -e
[ "$rc_delta" -ne 0 ] && grep -q 'timeout aguardando lock' "$TMP/delta.out"
[ "$rc_ice" -ne 0 ] && grep -q 'timeout aguardando lock' "$TMP/ice.out"
echo "table_lock_test ok"
