#!/usr/bin/env sh
# Interoperabilidade Delta Lake com pyarrow:
#  1. o tilt grava uma tabela delta particionada por [estado, ano] (layout
#     hive-style <c1>=<v1>/<c2>=<v2>, colunas de particao fora do parquet,
#     partitionValues no _delta_log);
#  2. o pyarrow le o diretorio como dataset hive-partitioned e confere o
#     schema, os valores reidratados e os tipos;
#  3. o tilt le de volta com pruning (onde:) e predicado residual, conferindo
#     contagens (pyarrow filtra o dataset e deve bater).
set -eu

BIN="$1"
FIXTURE="${2:-${0%/*}/fixtures/delta_particionado.tilt}"

command -v python3 >/dev/null 2>&1 || {
  echo "python3 ausente; pulando o teste delta"
  exit 0
}
python3 -c "import pyarrow" 2>/dev/null || {
  echo "pyarrow ausente; pulando o teste delta"
  exit 0
}

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# --- 1+3. tilt: escrita particionada + leitura com pruning ----------------------
out=$(cd "$tmp" && "$BIN" executar "$FIXTURE")
printf '%s\n' "$out"

fail=0
confere() {
  printf '%s\n' "$out" | grep -qF "$1" || { echo "saida sem '$1'"; fail=1; }
}
confere "total: 5"
confere "sp2024: 2"
confere "residual: 1"

# --- 2. pyarrow valida o layout hive e a reidratacao -----------------------------
python3 - "$tmp/vendas_delta" <<'PYEOF'
import sys
import pyarrow.dataset as ds
import pyarrow.compute as pc

d = ds.dataset(sys.argv[1], partitioning="hive")
t = d.to_table()
n = t.num_rows
assert n == 5, "esperado 5 linhas, obtido %d" % n
nomes = t.schema.names
assert "estado" in nomes and "ano" in nomes, "colunas de particao ausentes: %s" % nomes
est = t.column("estado").to_pylist()
ano = t.column("ano").to_pylist()
val = t.column("valor").to_pylist()
cid = t.column("cidade").to_pylist()
assert sorted(est) == ["rj", "rj", "sp", "sp", "sp"], est
assert sorted(ano) == [2024, 2024, 2024, 2025, 2025], ano
assert sum(val) == 180, val
# equivalencia com o pruning do tilt: estado=sp AND ano=2024 -> 2 linhas
# (o writer tilt grava texto como BYTE_ARRAY com anotacao UTF8, entao o
# pyarrow le `cidade` como string)
sel = t.filter((pc.field("estado") == "sp") & (pc.field("ano") == 2024))
assert sel.num_rows == 2, sel.num_rows
assert sorted(sel.column("cidade").to_pylist()) == ["santos", "sorocaba"]
# predicado residual em coluna comum: cidade=niteroi -> 1 linha
sel2 = t.filter(pc.field("cidade") == "niteroi")
assert sel2.num_rows == 1, sel2.num_rows
assert sel2.column("estado").to_pylist() == ["rj"]
print("pyarrow: layout hive, reidratacao e filtros validados (%d linhas)" % n)
PYEOF

[ "$fail" = 0 ] || exit "$fail"

# --- 4. checkpoint tilt-native a cada 10 versoes (Fase 12-5a) --------------------
cat > "$tmp/cp.tilt" <<'TILTEOF'
pipeline principal:
  passos:
    - base = [{ id: 1, v: "a" }]
    - escrever_delta base, "cp"
    - para cada i em [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11]:
        - nova = [{ id: i, v: "x" }]
        - anexar_delta nova, "cp"
    - tudo = ler_delta "cp"
    - imprimir "total: ", tamanho tudo
TILTEOF
out_cp=$(cd "$tmp" && "$BIN" executar cp.tilt)
printf '%s\n' "$out_cp"
echo "$out_cp" | grep -qE "total: +12" || { echo "checkpoint: total errado"; fail=1; }
[ -f "$tmp/cp/_delta_log/00000000000000000010.checkpoint.parquet" ] || {
  echo "checkpoint v10 nao materializado"; ls "$tmp/cp/_delta_log"; fail=1
}
# releitura usa o checkpoint como base + replay da cauda
cat > "$tmp/cp_ler.tilt" <<'TILTEOF'
pipeline principal:
  passos:
    - tudo = ler_delta "cp"
    - imprimir "releitura: ", tamanho tudo
    - so1 = ler_delta "cp", onde: { id: 1 }
    - imprimir "filtro: ", tamanho so1
TILTEOF
out_cp2=$(cd "$tmp" && "$BIN" executar cp_ler.tilt)
printf '%s\n' "$out_cp2"
echo "$out_cp2" | grep -qE "releitura: +12" || { echo "checkpoint: releitura errada"; fail=1; }
echo "$out_cp2" | grep -qE "filtro: +2" || { echo "checkpoint: filtro errado"; fail=1; }
[ "$fail" = 0 ] && echo "delta_test ok"
exit "$fail"
