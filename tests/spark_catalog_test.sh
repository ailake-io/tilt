#!/usr/bin/env sh
# Validacao de ouro do `tilt servir-catalogo` (fase 30): o tilt grava duas
# tabelas Iceberg num dir temporario (tests/fixtures/iceberg_catalogo.tilt —
# write+append sem particao e uma particionada por estado), sobe
# `tilt servir-catalogo` no host e um container apache/spark:3.5.3 configura um
# SparkCatalog tipo "rest" com a URI do servidor, lendo via
# spark.read.table("tiltcat.default.<tabela>") — confere contagens, valores de
# linhas conhecidas, coluna de particao reidratada e pruning. O metadata vem
# via REST (loadTable do tilt servir-catalogo); manifest lists, manifests e
# data files sao lidos pelo Spark via file:// absolutos (o fs.http do Hadoop
# reporta length -1 e o leitor Avro do Iceberg rejeita length < 4 sem ler o
# arquivo — por isso o servidor sobe com --sem-reecrita-manifests) — o tmp eh
# montado no MESMO path dentro do container (--network host: o container
# alcanca o catalogo em 127.0.0.1 do host). Requer docker; a 1a execucao tambem
# precisa de rede para baixar a imagem e o pacote iceberg-spark-runtime do
# Maven Central (--packages). Sem docker (ou sem rede), pula com mensagem —
# padrao dos testes de integracao do projeto.
set -eu

BIN="$1"
FIXTURE="${2:-${0%/*}/fixtures/iceberg_catalogo.tilt}"
case "$FIXTURE" in
  /*) ;;
  *) FIXTURE="$(pwd)/$FIXTURE" ;;
esac
PORT_BASE="${TILT_TEST_PORT:-8891}"

command -v docker >/dev/null 2>&1 || {
  echo "docker ausente; pulando o teste spark_catalog"
  exit 0
}
docker info >/dev/null 2>&1 || {
  echo "docker indisponivel; pulando o teste spark_catalog"
  exit 0
}

IMAGEM="apache/spark:3.5.3"
# pacote Maven do Iceberg para o Spark 3.5 (catalogo REST + leitura)
PKGS="org.apache.iceberg:iceberg-spark-runtime-3.5_2.12:1.6.1"

if ! docker image inspect "$IMAGEM" >/dev/null 2>&1; then
  echo "baixando $IMAGEM (primeira execucao)..."
  docker pull "$IMAGEM" || {
    echo "sem rede para baixar $IMAGEM; pulando o teste spark_catalog"
    exit 0
  }
fi

tmp=$(mktemp -d)
srv=""
trap 'kill "$srv" 2>/dev/null || true; rm -rf "$tmp"' EXIT

# --- 1. tilt grava as tabelas + sobe o catalogo REST --------------------------------
out=$(cd "$tmp" && "$BIN" executar "$FIXTURE")
printf '%s\n' "$out"
echo "$out" | grep -q "catalogo pronto" || {
  echo "fixture nao produziu o esperado: $out"
  exit 1
}

# readiness: "escutando" no log (prova que e o nosso processo que boundou) + config OK.
# --sem-reecrita-manifests: o Spark le o manifest list pelo fs.http do Hadoop,
# que reporta length -1 (o leitor Avro do Iceberg rejeita length < 4 sem ler o
# arquivo) — com o flag as manifest-lists ficam file:// absolutos e o container
# as resolve pelo mount no mesmo path.
PORTA=""
for p in $(seq "$PORT_BASE" $((PORT_BASE + 19))); do
  "$BIN" servir-catalogo "$tmp" --porta "$p" --sem-reecrita-manifests >"$tmp/catalogo.log" 2>&1 &
  srv=$!
  for _ in $(seq 1 50); do
    if ! kill -0 "$srv" 2>/dev/null; then break; fi
    if grep -q "escutando" "$tmp/catalogo.log" 2>/dev/null &&
       curl -sf "http://127.0.0.1:$p/v1/config" >/dev/null 2>&1; then
      PORTA=$p
      break 2
    fi
    sleep 0.1
  done
  kill "$srv" 2>/dev/null || true
  wait "$srv" 2>/dev/null || true
  srv=""
done
[ -n "$PORTA" ] || {
  echo "servir-catalogo nao iniciou a partir da porta $PORT_BASE"
  cat "$tmp/catalogo.log"
  exit 1
}
echo "catalogo REST no ar: http://127.0.0.1:$PORTA/v1"

# --- 2. spark (container, --network host) le via SparkCatalog rest -------------------
cat > "$tmp/verifica_catalogo.py" <<'PYEOF'
import sys

from pyspark.sql import SparkSession
from pyspark.sql import functions as F

uri = sys.argv[1]

spark = (SparkSession.builder
         .appName("tilt-spark-catalogo-rest")
         .master("local[*]")
         .config("spark.sql.catalog.tiltcat", "org.apache.iceberg.spark.SparkCatalog")
         .config("spark.sql.catalog.tiltcat.type", "rest")
         .config("spark.sql.catalog.tiltcat.uri", uri)
         .getOrCreate())
spark.sparkContext.setLogLevel("ERROR")


def falha(msg):
    raise SystemExit("SPARK FALHA: " + msg)


# metadata + manifest-lists via REST catalog do tilt; manifests/data via file://
ice = spark.read.table("tiltcat.default.tabela_iceberg")
n = ice.count()
if n != 3:
    falha("tabela_iceberg: esperadas 3 linhas, lidas %d" % n)
if ice.columns != ["nome", "idade", "nota"]:
    falha("tabela_iceberg: schema divergente: %r" % (ice.columns,))
linhas = {r["nome"]: (r["idade"], r["nota"]) for r in ice.collect()}
esperado = {"ana": (30, 9.5), "bruno": (40, 7.0), "carla": (50, 8.0)}
if linhas != esperado:
    falha("tabela_iceberg: linhas divergem: %r" % (linhas,))
print("SPARK OK tabela_iceberg (%d linhas via catalogo REST, write+append)" % n)

vp = spark.read.table("tiltcat.default.vendas_part")
n = vp.count()
if n != 3:
    falha("vendas_part: esperadas 3 linhas, lidas %d" % n)
if vp.columns != ["estado", "cidade", "valor"]:
    falha("vendas_part: schema divergente: %r" % (vp.columns,))
linhas = {r["cidade"]: (r["estado"], r["valor"]) for r in vp.collect()}
esperado = {"santos": ("sp", 10), "campinas": ("sp", 30), "niteroi": ("rj", 20)}
if linhas != esperado:
    falha("vendas_part: linhas divergem: %r" % (linhas,))
# pruning real: predicate na coluna de particao poda pelos summaries do
# manifest list (bounds) — reidrata estado a partir do record `partition`
sp = vp.filter(F.col("estado") == "sp").count()
if sp != 2:
    falha("vendas_part: pruning estado=sp deveria achar 2 linhas, achou %d" % sp)
mg = vp.filter(F.col("estado") == "mg").count()
if mg != 0:
    falha("vendas_part: estado=mg fora dos bounds deveria achar 0, achou %d" % mg)
print("SPARK OK vendas_part (%d linhas, particao identity reidratada, pruning)" % n)

spark.stop()
PYEOF

# --network host: o container compartilha a rede do host (alcanca o catalogo em
# 127.0.0.1:PORTA); -v monta o tmp no mesmo path absoluto (manifests apontam
# file:// absolutos para manifests/data files)
if ! docker run --rm --user root \
       --network host \
       -v "$tmp:$tmp" \
       "$IMAGEM" \
       /opt/spark/bin/spark-submit --master "local[*]" \
         --packages "$PKGS" \
         "$tmp/verifica_catalogo.py" "http://127.0.0.1:$PORTA" > "$tmp/spark.log" 2>&1; then
  echo "spark-submit falhou (tail do log):"
  tail -40 "$tmp/spark.log"
  echo "--- log do catalogo:"
  tail -20 "$tmp/catalogo.log"
  exit 1
fi

grep -q "^SPARK OK tabela_iceberg" "$tmp/spark.log" || {
  echo "leitura da tabela_iceberg nao confirmada (tail do log):"
  tail -30 "$tmp/spark.log"
  exit 1
}
grep -q "^SPARK OK vendas_part" "$tmp/spark.log" || {
  echo "leitura da vendas_part nao confirmada (tail do log):"
  tail -30 "$tmp/spark.log"
  exit 1
}
grep -E '^SPARK OK' "$tmp/spark.log"

echo "spark_catalog_test ok"
