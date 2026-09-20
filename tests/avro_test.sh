#!/usr/bin/env sh
set -eu

BIN="$1"
tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT
tmp="$tmpdir/avro.tilt"
cat > "$tmp" <<'EOF'
pipeline principal:
  passos:
    - schema = """{"type":"record","name":"Pessoa","fields":[{"name":"id","type":"long"},{"name":"nome","type":"string"}]}"""
    - payload = avro_codificar { id: 7, nome: "Ana" }, schema, 42
    - dec = avro_decodificar payload, schema
    - imprimir dec.id_esquema, dec.valor.id, dec.valor.nome
EOF

"$BIN" checar "$tmp" >/dev/null
out=$("$BIN" executar "$tmp")
case "$out" in
  *"42 7 Ana"*) ;;
  *) exit 1 ;;
esac
printf '%s' 'avro: Confluent wire round-trip ok'
