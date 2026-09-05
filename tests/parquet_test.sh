#!/usr/bin/env sh
# Interoperabilidade Parquet com pyarrow:
#  1. gera interop.parquet com pyarrow: varios row groups (row_group_size=5),
#     dictionary encoding (use_dictionary=True), compressao gzip e colunas
#     opcionais com nulos;
#  2. le com `tilt executar` e confere os valores impressos (multi row group
#     + dictionary + gzip + definition levels na leitura);
#  3. o mesmo pipeline grava saida_nulos.parquet com nulos pelo tilt e o
#     pyarrow valida o arquivo (campos OPTIONAL, posicoes dos nulos, valores).
set -eu

BIN="$1"
FIXTURE="${2:-${0%/*}/fixtures/parquet_interop.tilt}"

command -v python3 >/dev/null 2>&1 || {
  echo "python3 ausente; pulando o teste parquet"
  exit 0
}
python3 -c "import pyarrow" 2>/dev/null || {
  echo "pyarrow ausente; pulando o teste parquet"
  exit 0
}
python3 -c "import ctypes; ctypes.CDLL('libz.so.1')" 2>/dev/null || {
  echo "libz.so.1 ausente; pulando o teste parquet"
  exit 0
}

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# --- 1. arquivo pyarrow: multi row groups + dictionary + gzip + nulos -----------
python3 - "$tmp/interop.parquet" <<'PYEOF'
import sys
import pyarrow as pa
import pyarrow.parquet as pq

n = 23
tabela = pa.table({
    "id": pa.array(list(range(n)), type=pa.int64()),
    "nome": pa.array([("cliente-%d" % (i % 5)) if i % 7 != 3 else None
                      for i in range(n)], type=pa.string()),
    "score": pa.array([round(i * 1.5, 2) if i % 4 != 1 else None
                       for i in range(n)], type=pa.float64()),
    "ativo": pa.array([bool(i % 2) for i in range(n)], type=pa.bool_()),
})
pq.write_table(tabela, sys.argv[1], row_group_size=5,
               use_dictionary=True, compression="gzip")

f = pq.ParquetFile(sys.argv[1])
assert f.metadata.num_row_groups >= 3, "esperado >= 3 row groups"
for rg in range(f.metadata.num_row_groups):
    for c in range(f.metadata.num_columns):
        col = f.metadata.row_group(rg).column(c)
        assert col.compression == "GZIP", "esperado codec gzip"
        if col.path_in_schema in ("id", "nome", "score"):
            encs = col.encodings
            assert any("DICTIONARY" in e for e in encs), \
                "esperado dictionary encoding em " + col.path_in_schema
assert f.schema.column(0).max_definition_level == 1, "esperado colunas opcionais"
print("pyarrow: arquivo gerado (row groups=%d, dictionary, gzip, nulos)"
      % f.metadata.num_row_groups)
PYEOF

# --- 2. leitura pelo tilt ------------------------------------------------------
out=$(cd "$tmp" && "$BIN" executar "$FIXTURE")
printf '%s\n' "$out"

fail=0
confere() {
  printf '%s\n' "$out" | grep -qF "$1" || { echo "saida sem '$1'"; fail=1; }
}
confere "linhas: 23"
confere "0 cliente-0 0 falso"
confere "3 nulo 4.5 verdadeiro"
confere "17 nulo nulo verdadeiro"
confere "22 cliente-2 33 falso"
confere "aa 1 9.9"
confere "nulo nulo 4.5"
confere "cc 7 nulo"

# --- 3. roundtrip de escrita do tilt validado pelo pyarrow ---------------------
python3 - "$tmp/saida_nulos.parquet" <<'PYEOF'
import sys
import pyarrow.parquet as pq

t = pq.read_table(sys.argv[1]).to_pydict()
assert t["rotulo"] == [b"aa", None, b"cc"], t["rotulo"]
assert t["qtd"] == [1, None, 7], t["qtd"]
assert t["preco"] == [9.9, 4.5, None], t["preco"]
esquema = pq.ParquetFile(sys.argv[1]).schema
for i in range(len(esquema.names)):
    assert esquema.column(i).max_definition_level == 1, \
        "coluna " + esquema.column(i).name + " deveria ser OPTIONAL"
print("pyarrow: saida_nulos.parquet validado (OPTIONAL, nulos e valores ok)")
PYEOF

[ "$fail" = 0 ] && echo "parquet_test ok"
exit "$fail"
