#!/usr/bin/env sh
set -eu

bin=$1
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
printf 'pesos-v1' >"$dir/modelo.safetensors"
"$bin" registrar-modelo churn "$dir/modelo.safetensors" --versao 1 --registro "$dir/registro" >/dev/null
"$bin" listar-modelos --registro "$dir/registro" | grep -qx 'churn@1'
grep -q '"sha256"' "$dir/registro/churn/1/manifest.json"
printf 'pesos-v2' >"$dir/modelo.safetensors"
"$bin" registrar-modelo churn "$dir/modelo.safetensors" --versao 2 --registro "$dir/registro" >/dev/null
"$bin" listar-modelos --registro "$dir/registro" | grep -qx 'churn@1'
"$bin" listar-modelos --registro "$dir/registro" | grep -qx 'churn@2'
"$bin" promover-modelo churn 2 production --registro "$dir/registro" >/dev/null
grep -q '"stage": "production"' "$dir/registro/churn/2/manifest.json"
echo "model_registry_test ok"
