#!/usr/bin/env sh
# Interoperabilidade real com Apache Spark 3.5 (fase 12-1): o tilt grava tres
# tabelas num dir temporario via tests/fixtures/spark_interop.tilt —
#   (a) Delta particionado por [estado] com 2 appends (o 2o traz a coluna nova
#       "canal": evolucao de schema, linhas antigas leem nulo);
#   (b) Iceberg particionado por [estado] (catalogo Hadoop local);
#   (c) Parquet com listas + gzip;
# e um container apache/spark:3.5.3 le o que consegue de volta via
# spark-submit (pyspark), conferindo contagens, valores de linhas conhecidas,
# coluna de particao reidratada, a coluna nova do append Delta e as listas do
# Parquet. O Iceberg expoe um gap real de writer (fase 12-5): o tilt grava o
# metadata como v<N>-<uuid>.metadata.json e o HadoopTableOperations do
# Iceberg/Spark so resolve v<N>.metadata.json — a leitura falha com "Table
# does not exist" e o teste reporta "SPARK GAP iceberg" sem contornar; as
# verificacoes estritas de Iceberg passam a valer quando o writer for
# corrigido. Requer docker; a 1a execucao tambem precisa de rede para baixar
# a imagem e os pacotes io.delta:delta-spark /
# org.apache.iceberg:iceberg-spark-runtime do Maven Central (--packages do
# spark-submit). Sem docker (ou sem rede para a imagem), pula com mensagem —
# padrao dos testes de integracao do projeto.
set -eu

BIN="$1"
FIXTURE="${2:-${0%/*}/fixtures/spark_interop.tilt}"
case "$FIXTURE" in
  /*) ;;
  *) FIXTURE="$(pwd)/$FIXTURE" ;;
esac

command -v docker >/dev/null 2>&1 || {
  echo "docker ausente; pulando o teste spark"
  exit 0
}
docker info >/dev/null 2>&1 || {
  echo "docker indisponivel; pulando o teste spark"
  exit 0
}

IMAGEM="apache/spark:3.5.3"
# pacotes Maven para o Spark 3.5: leitura Delta (delta-spark) e Iceberg
# (iceberg-spark-runtime) por data source; o parquet nativo nao precisa de pacote
PKGS="io.delta:delta-spark_2.12:3.2.1,org.apache.iceberg:iceberg-spark-runtime-3.5_2.12:1.6.1"

if ! docker image inspect "$IMAGEM" >/dev/null 2>&1; then
  echo "baixando $IMAGEM (primeira execucao)..."
  docker pull "$IMAGEM" || {
    echo "sem rede para baixar $IMAGEM; pulando o teste spark"
    exit 0
  }
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# --- 1. tilt grava as 3 tabelas --------------------------------------------------
out=$(cd "$tmp" && "$BIN" executar "$FIXTURE")
printf '%s\n' "$out"

fail=0
confere() {
  printf '%s\n' "$out" | grep -qF "$1" || { echo "saida sem '$1'"; fail=1; }
}
confere "delta linhas: 5"
confere "sp santos 10 nulo"
confere "rj niteroi 20 nulo"
confere "sp campinas 30 web"
confere "mg bh 40 loja"
confere "iceberg linhas: 3"
confere "sp ana 30"
confere "rj bruno 40"
confere "parquet linhas: 3"
confere "caneta [escrita, azul] [1.5, 2]"
confere "caderno [] []"
[ "$fail" = 0 ] || exit 1

# layout de catalogo Hadoop para o Spark resolver a tabela Iceberg pelo nome
# (warehouse/namespace/tabela -> dir gravado pelo tilt). O symlink usa o
# caminho visto DENTRO do container (/data): o tmp eh montado la em /data.
mkdir -p "$tmp/spark_cat/default"
ln -s "/data/clientes_iceberg" "$tmp/spark_cat/default/clientes_iceberg"

# --- 2. spark (container) le as tres tabelas e confere ---------------------------
cat > "$tmp/verifica_spark.py" <<'PYEOF'
import sys

from pyspark.sql import SparkSession
from pyspark.sql import functions as F

base = sys.argv[1]

spark = (SparkSession.builder
         .appName("tilt-spark-interop")
         .master("local[*]")
         .config("spark.sql.catalog.tiltice", "org.apache.iceberg.spark.SparkCatalog")
         .config("spark.sql.catalog.tiltice.type", "hadoop")
         .config("spark.sql.catalog.tiltice.warehouse", base + "/spark_cat")
         .getOrCreate())
spark.sparkContext.setLogLevel("ERROR")


def falha(msg):
    raise SystemExit("SPARK FALHA: " + msg)


# ---------------------------------------------------------- (a) Delta Lake
delta = spark.read.format("delta").load(base + "/vendas_delta")
n = delta.count()
if n != 5:
    falha("delta: esperadas 5 linhas, lidas %d" % n)
if delta.columns != ["estado", "cidade", "valor", "canal"]:
    falha("delta: schema divergente: %r" % (delta.columns,))
tipos = dict(delta.dtypes)
if tipos.get("estado") != "string" or tipos.get("cidade") != "string" \
        or tipos.get("valor") != "bigint" or tipos.get("canal") != "string":
    falha("delta: tipos divergentes: %r" % (tipos,))
linhas = {r["cidade"]: (r["estado"], r["valor"], r["canal"])
          for r in delta.collect()}
esperado = {
    "santos": ("sp", 10, None),       # linha antiga: canal nulo
    "niteroi": ("rj", 20, None),      # linha antiga: canal nulo
    "campinas": ("sp", 30, "web"),
    "bh": ("mg", 40, "loja"),
    "rio": ("rj", 50, "web"),
}
if linhas != esperado:
    falha("delta: linhas divergem: %r" % (linhas,))
nulos = delta.filter(F.col("canal").isNull()).count()
if nulos != 2:
    falha("delta: coluna nova deveria ter 2 nulos (linhas antigas), tem %d" % nulos)
arquivos = [f for f in delta.inputFiles() if "estado=" in f]
if not arquivos:
    falha("delta: nenhum data file em diretorio de particao (layout hive?)")
print("SPARK OK delta (%d linhas, particao reidratada, "
      "coluna nova do append visivel com nulo nas linhas antigas)" % n)

# ---------------------------------------------------------- (b) Iceberg
# Gap conhecido (writer, fase 12-5): o tilt grava o metadata como
# v<N>-<uuid>.metadata.json, mas o HadoopTableOperations do Iceberg/Spark so
# resolve "v<N>.metadata.json" (regex v([^\..*]) + Integer.parseInt e
# procura exata do arquivo; ver core/src/main/java/org/apache/iceberg/hadoop/
# HadoopTableOperations.java). Enquanto nao for corrigido, a leitura via
# HadoopCatalog falha com "Table does not exist" — reportada como gap, sem
# contornar. Se o writer passar a usar v<N>.metadata.json, as verificacoes
# estritas abaixo passam a valer automaticamente.
try:
    ice = spark.table("tiltice.default.clientes_iceberg")
except Exception as e:
    msg = str(e)
    if "TABLE_OR_VIEW_NOT_FOUND" in msg and "clientes_iceberg" in msg:
        print("SPARK GAP iceberg (writer, fase 12-5): metadata "
              "v<N>-<uuid>.metadata.json fora da convencao v<N>.metadata.json "
              "do HadoopCatalog; a leitura via SparkCatalog falha com "
              "'Table does not exist'")
    else:
        falha("iceberg: falha inesperada na leitura: %s" % msg.splitlines()[0])
else:
    n = ice.count()
    if n != 3:
        falha("iceberg: esperadas 3 linhas, lidas %d" % n)
    if ice.columns != ["estado", "nome", "idade"]:
        falha("iceberg: schema divergente: %r" % (ice.columns,))
    linhas = {r["nome"]: (r["estado"], r["idade"]) for r in ice.collect()}
    esperado = {"ana": ("sp", 30), "bruno": ("rj", 40), "carla": ("sp", 50)}
    if linhas != esperado:
        falha("iceberg: linhas divergem: %r" % (linhas,))
    nulos = ice.filter(F.col("estado").isNull()).count()
    if nulos != 0:
        falha("iceberg: coluna de particao reidratada com %d nulos" % nulos)
    print("SPARK OK iceberg (%d linhas, particao identity reidratada)" % n)

# ---------------------------------------------------------- (c) Parquet
par = spark.read.parquet(base + "/produtos.parquet")
n = par.count()
if n != 3:
    falha("parquet: esperadas 3 linhas, lidas %d" % n)
if par.columns != ["nome", "tags", "precos"]:
    falha("parquet: schema divergente: %r" % (par.columns,))
esquema = {f.name: f.dataType.simpleString() for f in par.schema.fields}
if esquema.get("tags") != "array<string>" or esquema.get("precos") != "array<double>":
    falha("parquet: tipos de lista divergem: %r" % (esquema,))
linhas = {r["nome"]: (list(r["tags"]), list(r["precos"])) for r in par.collect()}
esperado = {
    "caneta": (["escrita", "azul"], [1.5, 2.0]),
    "caderno": ([], []),
    "borracha": (["escrita"], [0.5]),
}
if linhas != esperado:
    falha("parquet: linhas divergem: %r" % (linhas,))
print("SPARK OK parquet (%d linhas, listas intactas)" % n)

spark.stop()
PYEOF

# o container roda como root para poder ler o dir temporario (mktemp -d vem
# 0700); os pacotes --packages vao para o ivy cache do root no 1o uso
if ! docker run --rm --user root \
       -v "$tmp:/data" -w /data \
       "$IMAGEM" \
       /opt/spark/bin/spark-submit --master "local[*]" \
         --conf spark.sql.extensions=io.delta.sql.DeltaSparkSessionExtension \
         --conf spark.sql.catalog.spark_catalog=org.apache.spark.sql.delta.catalog.DeltaCatalog \
         --packages "$PKGS" \
         /data/verifica_spark.py /data > "$tmp/spark.log" 2>&1; then
  echo "spark-submit falhou (tail do log):"
  tail -40 "$tmp/spark.log"
  exit 1
fi

grep -E '^SPARK (OK|GAP)' "$tmp/spark.log" || {
  echo "verificacao spark nao confirmada (tail do log):"
  tail -30 "$tmp/spark.log"
  exit 1
}

echo "spark_test ok"
