#!/usr/bin/env sh
# Exercita os quatro backends vetoriais contra servicos reais provisionados
# pelo job vector_connectors do CI. Fora do CI, a ausencia de qualquer servico
# faz o teste pular, preservando o ctest local sem Docker.
set -eu

BIN="$1"
FIXTURES="$2"

command -v curl >/dev/null 2>&1 || {
  echo "curl ausente; pulando vector_connectors_real"
  exit 0
}

wait_http() {
  url="$1"
  for _ in $(seq 1 "${TILT_VECTOR_WAIT_ATTEMPTS:-15}"); do
    if curl -fsS --max-time 2 "$url" >/dev/null 2>&1; then return 0; fi
    sleep 1
  done
  return 1
}

for endpoint in \
  "http://127.0.0.1:6333/readyz" \
  "http://127.0.0.1:8080/v1/.well-known/ready" \
  "http://127.0.0.1:8000/api/v1/heartbeat"; do
  if ! wait_http "$endpoint"; then
    echo "servico vetorial ausente em $endpoint; pulando vector_connectors_real"
    exit 0
  fi
done

if ! command -v pg_isready >/dev/null 2>&1 || \
   ! pg_isready -h 127.0.0.1 -p 5433 -U postgres -d vdb >/dev/null 2>&1; then
  echo "pgvector/Postgres ausente em 127.0.0.1:5433; pulando vector_connectors_real"
  exit 0
fi

run_fixture() {
  env TILT_LLM=mock "$BIN" executar "$1"
}

check_common() {
  output="$1"
  echo "$output" | grep -q "inseridos: 3" || {
    echo "vector_connectors_real: insercao incompleta: $output"
    return 1
  }
  [ "$(printf '%s\n' "$output" | grep -c '^achado:')" = 2 ] || {
    echo "vector_connectors_real: busca nao retornou 2 hits: $output"
    return 1
  }
  # o texto guardado no `inserir` volta no `buscar` (todos os backends)
  echo "$output" | grep -q '^texto1: gato' || {
    echo "vector_connectors_real: hit sem o texto inserido: $output"
    return 1
  }
}

qdrant_out=$(run_fixture "$FIXTURES/qdrant_rag.tilt")
check_common "$qdrant_out"
echo "$qdrant_out" | grep -q "^top1: " || {
  echo "qdrant nao retornou top1: $qdrant_out"; exit 1;
}
qdrant_info=$(curl -fsS http://127.0.0.1:6333/collections/docs)
echo "$qdrant_info" | grep -q '"points_count":3' || {
  echo "qdrant nao confirmou 3 pontos: $qdrant_info"; exit 1;
}

pgvector_out=$(run_fixture "$FIXTURES/pgvector_rag.tilt")
check_common "$pgvector_out"
echo "$pgvector_out" | grep -q "top1: b1" || {
  echo "pgvector ranking inesperado: $pgvector_out"; exit 1;
}
pg_count=$(PGPASSWORD=tilt psql -h 127.0.0.1 -p 5433 -U postgres -d vdb -At \
  -c "select count(*) from docs")
[ "$pg_count" = 3 ] || {
  echo "pgvector nao confirmou 3 linhas: $pg_count"; exit 1;
}

weaviate_out=$(run_fixture "$FIXTURES/weaviate_real_rag.tilt")
check_common "$weaviate_out"
echo "$weaviate_out" | grep -q "top1: 00000000-0000-4000-8000-000000000002" || {
  echo "weaviate ranking inesperado: $weaviate_out"; exit 1;
}

chroma_out=$(run_fixture "$FIXTURES/chroma_rag.tilt")
check_common "$chroma_out"
echo "$chroma_out" | grep -q "top1: b1" || {
  echo "chroma ranking inesperado: $chroma_out"; exit 1;
}

echo "vector_connectors_real ok (qdrant, weaviate, chroma, pgvector)"
exit 0
