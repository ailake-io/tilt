#!/usr/bin/env sh
# Interoperabilidade Parquet com pyarrow:
#  1. gera interop.parquet com pyarrow: varios row groups (row_group_size=5),
#     dictionary encoding (use_dictionary=True), compressao gzip e colunas
#     opcionais com nulos;
#  2. le com `tilt executar` e confere os valores impressos (multi row group
#     + dictionary + gzip + definition levels na leitura);
#  3. o mesmo pipeline grava saida_nulos.parquet com nulos pelo tilt e o
#     pyarrow valida o arquivo (campos OPTIONAL, posicoes dos nulos, valores,
#     strings como UTF8/string);
#  4. tilt grava com codec: "snappy" (compressor literal-only) e o pyarrow
#     valida (compression == SNAPPY, valores, listas);
#  5. pyarrow grava com compression="snappy" (matching real) e o tilt le;
#  6. DATA_PAGE_V2 nos dois sentidos (paginas: "v2" no tilt,
#     data_page_version="2.0" no pyarrow);
#  7. colunas de lista (list<string>, list<int64>) nos dois sentidos.
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
assert t["rotulo"] == ["aa", None, "cc"], t["rotulo"]  # UTF8 -> string
assert t["qtd"] == [1, None, 7], t["qtd"]
assert t["preco"] == [9.9, 4.5, None], t["preco"]
esquema = pq.ParquetFile(sys.argv[1]).schema
assert esquema.column(0).logical_type.type == "STRING", "esperado UTF8 em string"
for i in range(len(esquema.names)):
    assert esquema.column(i).max_definition_level == 1, \
        "coluna " + esquema.column(i).name + " deveria ser OPTIONAL"
print("pyarrow: saida_nulos.parquet validado (UTF8/string, OPTIONAL, nulos e valores ok)")
PYEOF

# --- 4. tilt grava snappy (literal-only) -> pyarrow valida ---------------------
cat > "$tmp/escrita_snappy.tilt" <<'TILTEOF'
pipeline escrita:
  passos:
    - tabela = [
        { nome: "ana", tags: ["ml", "dados"], nums: [1, 2, 3] },
        { nome: "bruno", tags: [], nums: [] },
        { nome: nulo, tags: nulo, nums: nulo }
      ]
    - escrever_parquet tabela, "saida_snappy.parquet", codec: "snappy"
    - escrever_parquet tabela, "saida_snappy_v2.parquet", codec: "snappy", paginas: "v2"
TILTEOF
(cd "$tmp" && "$BIN" executar escrita_snappy.tilt >/dev/null)

python3 - "$tmp/saida_snappy.parquet" "$tmp/saida_snappy_v2.parquet" <<'PYEOF'
import sys
import pyarrow.parquet as pq

esperado = {
    "nome": ["ana", "bruno", None],
    "tags": [["ml", "dados"], [], None],
    "nums": [[1, 2, 3], [], None],
}
for path, ver in ((sys.argv[1], "1.0"), (sys.argv[2], "2.0")):
    f = pq.ParquetFile(path)
    for c in range(f.metadata.num_columns):
        col = f.metadata.row_group(0).column(c)
        assert col.compression == "SNAPPY", "esperado codec snappy"
    assert pq.read_table(path).to_pydict() == esperado, path
    print("pyarrow: %s validado (snappy, paginas v%s, listas e valores ok)"
          % (path.split("/")[-1], ver))
PYEOF

# --- 5. pyarrow grava snappy (matching real) -> tilt le ------------------------
python3 - "$tmp/py_snappy.parquet" <<'PYEOF'
import sys
import pyarrow as pa
import pyarrow.parquet as pq

tabela = pa.table({
    "nome": pa.array(["ana", "bruno", "carla", "ana"], type=pa.string()),
    "nums": pa.array([[1, 2], None, [3], []], type=pa.list_(pa.int64())),
})
# snappy com matching real (strings repetidas comprimem com back-references)
pq.write_table(tabela, sys.argv[1], compression="snappy", use_dictionary=False)
assert pq.ParquetFile(sys.argv[1]).metadata.row_group(0).column(0).compression == "SNAPPY"
print("pyarrow: py_snappy.parquet gerado (snappy, sem dictionary)")
PYEOF

cat > "$tmp/leitura_snappy.tilt" <<'TILTEOF'
pipeline leitura:
  passos:
    - dados = ler_parquet "py_snappy.parquet"
    - para cada linha em dados:
        - imprimir linha.nome, linha.nums
TILTEOF
out=$(cd "$tmp" && "$BIN" executar leitura_snappy.tilt)
printf '%s\n' "$out"
confere "ana [1, 2]"
confere "bruno nulo"
confere "carla [3]"
confere "ana []"

# --- 6. DATA_PAGE_V2 nos dois sentidos ------------------------------------------
python3 - "$tmp/py_v2.parquet" <<'PYEOF'
import sys
import pyarrow as pa
import pyarrow.parquet as pq

tabela = pa.table({
    "nome": pa.array(["ana", "bruno", None], type=pa.string()),
    "qtd": pa.array([10, None, 30], type=pa.int64()),
})
pq.write_table(tabela, sys.argv[1], compression="gzip", data_page_version="2.0")
print("pyarrow: py_v2.parquet gerado (DATA_PAGE_V2, gzip)")
PYEOF

cat > "$tmp/leitura_v2.tilt" <<'TILTEOF'
pipeline leitura_v2:
  passos:
    - dados = ler_parquet "py_v2.parquet"
    - para cada linha em dados:
        - imprimir linha.nome, linha.qtd
    - volta = ler_parquet "saida_snappy_v2.parquet"
    - imprimir "tilt-v2:", volta[0].nome, volta[0].tags, volta[1].tags, volta[2].tags
TILTEOF
out=$(cd "$tmp" && "$BIN" executar leitura_v2.tilt)
printf '%s\n' "$out"
confere "ana 10"
confere "bruno nulo"
confere "nulo 30"
confere "tilt-v2: ana [ml, dados] [] nulo"

# --- 7. colunas de lista nos dois sentidos --------------------------------------
python3 - "$tmp/py_listas.parquet" <<'PYEOF'
import sys
import pyarrow as pa
import pyarrow.parquet as pq

tabela = pa.table({
    "tags": pa.array([["alpha", "beta"], [], None], type=pa.list_(pa.string())),
    "nums": pa.array([[7, 8], None, []], type=pa.list_(pa.int64())),
})
pq.write_table(tabela, sys.argv[1], compression="snappy")
f = pq.ParquetFile(sys.argv[1])
tipos = [str(f.schema_arrow.field(k).type) for k in range(2)]
assert tipos == ["list<element: string>", "list<element: int64>"], tipos
print("pyarrow: py_listas.parquet gerado (list<string>, list<int64>, snappy)")
PYEOF

cat > "$tmp/leitura_listas.tilt" <<'TILTEOF'
pipeline leitura_listas:
  passos:
    - dados = ler_parquet "py_listas.parquet"
    - para cada linha em dados:
        - imprimir linha.tags, linha.nums
TILTEOF
out=$(cd "$tmp" && "$BIN" executar leitura_listas.tilt)
printf '%s\n' "$out"
confere "[alpha, beta] [7, 8]"
confere "[] nulo"
confere "nulo []"

python3 - "$tmp/saida_snappy.parquet" <<'PYEOF'
import sys
import pyarrow.parquet as pq

f = pq.ParquetFile(sys.argv[1])
tipos = [str(f.schema_arrow.field(k).type) for k in range(f.metadata.num_columns)]
assert tipos == ["string", "list<element: string not null>", "list<element: int64 not null>"], tipos
t = pq.read_table(sys.argv[1]).to_pydict()
assert t["tags"] == [["ml", "dados"], [], None], t["tags"]
assert t["nums"] == [[1, 2, 3], [], None], t["nums"]
print("pyarrow: listas escritas pelo tilt validadas (tipos e valores ok)")
PYEOF

[ "$fail" = 0 ] && echo "parquet_test ok"
exit "$fail"
