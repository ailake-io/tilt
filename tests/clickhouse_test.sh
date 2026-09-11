#!/usr/bin/env sh
# Integration test do conector ClickHouse (HTTP nativo sobre o cliente
# generico do runtime): roda fixtures/clickhouse_roundtrip.tilt contra um
# servidor real — criando a tabela (MergeTree) via executar_sql
# "clickhouse://...", inserindo, lendo de volta via fonte/ler (Int64 ->
# inteiro, Float64 -> decimal, Nullable(String) -> nulo/texto, Date -> texto)
# e capturando os erros do servidor (SQL invalido e tabela inexistente).
# O servidor sobe de duas formas, na ordem:
#   1) clickhouse no PATH ou /usr/bin/clickhouse*
#   2) binario unico em <repo>/.cache/clickhouse/clickhouse (baixado uma unica
#      vez — mesmo CDN do `curl https://clickhouse.com/ | sh` — se nao existir)
# Se nenhuma estiver disponivel (e o download falhar), pula com mensagem
# (padrao dos outros testes). O servidor roda em diretorio temporario com
# http_port proprio a partir de TILT_TEST_PORT (default 8671); o proprio tilt
# tambem precisa do binario `curl` (o cliente HTTP generico e subprocesso curl).
set -eu

BIN="$1"
FIXTURE="${2:-${0%/*}/fixtures/clickhouse_roundtrip.tilt}"
PORT_BASE="${TILT_TEST_PORT:-8671}"

command -v curl >/dev/null 2>&1 || {
  echo "curl ausente; pulando o teste clickhouse"
  exit 0
}

# ---------------------------------------------------------------- descoberta
CLICKHOUSE_BIN=""
for dir in $(echo "$PATH" | tr ':' ' ') /usr/bin /usr/local/bin /opt/clickhouse/bin; do
  [ -n "$dir" ] || continue
  if [ -z "$CLICKHOUSE_BIN" ] && [ -x "$dir/clickhouse" ]; then
    CLICKHOUSE_BIN="$dir/clickhouse"
  fi
done

CACHE_BIN="${0%/*}/../.cache/clickhouse/clickhouse"
if [ -z "$CLICKHOUSE_BIN" ] && [ -x "$CACHE_BIN" ]; then
  CLICKHOUSE_BIN="$CACHE_BIN"
fi

# Download sob demanda (uma unica vez; .cache/ esta no .gitignore). Mesma
# logica do instalador oficial: https://clickhouse.com/ resolve a arquitetura
# para https://builds.clickhouse.com/master/<amd64|aarch64>/clickhouse.
if [ -z "$CLICKHOUSE_BIN" ]; then
  arch=$(uname -m 2>/dev/null || echo desconhecido)
  case "$arch" in
    x86_64|amd64) dir=amd64 ;;
    aarch64|arm64) dir=aarch64 ;;
    *)
      echo "clickhouse ausente e arquitetura '$arch' sem binario unico oficial; pulando o teste clickhouse"
      exit 0
      ;;
  esac
  echo "clickhouse ausente; baixando o binario unico ($dir) para .cache/clickhouse (uma unica vez)"
  mkdir -p "${CACHE_BIN%/*}"
  if curl -fSL --retry 2 -o "$CACHE_BIN.tmp" \
      "https://builds.clickhouse.com/master/$dir/clickhouse" 2>"${CACHE_BIN}.log"; then
    chmod +x "$CACHE_BIN.tmp"
    mv "$CACHE_BIN.tmp" "$CACHE_BIN"
    # o binario e estatico p/ Linux: confirma que executa antes de prosseguir
    if ! "$CACHE_BIN" --version >/dev/null 2>&1; then
      echo "binario baixado nao executa nesta maquina; pulando o teste clickhouse"
      rm -f "$CACHE_BIN"
      exit 0
    fi
    CLICKHOUSE_BIN="$CACHE_BIN"
  else
    echo "download do clickhouse falhou (veja ${CACHE_BIN}.log); pulando o teste clickhouse"
    exit 0
  fi
fi

tmp=$(mktemp -d)
srv_pid=""
trap 'kill "$srv_pid" 2>/dev/null || true; rm -rf "$tmp"' EXIT

# Caminho absoluto: o servidor sobe com CWD=$tmp (ver start_local), entao um
# caminho relativo nao resolveria mais.
case "$CLICKHOUSE_BIN" in
  /*) : ;;
  *) CLICKHOUSE_BIN="$(pwd)/$CLICKHOUSE_BIN" ;;
esac

# ------------------------------------------------------------- servidor local
# Subcomando `server` com overrides de config apos `--` (exigido pelas versoes
# atuais; sem o separador o parser rejeita "--path ..."). tcp/mysql/postgresql
# em 0 liberam as portas fixas (9000/9004/9005) para nao disputar com outros
# servidores de teste; as demais interfaces do servidor usam portas efemeras.
PORTA=""
start_local() {
  mkdir -p "$tmp/data"
  # CWD no tmp: alem do --path, o servidor escreve access/ e
  # preprocessed_configs/ no diretorio corrente — assim o trap limpa tudo.
  # exec para o $! ser o proprio processo (watcher) do clickhouse.
  ( cd "$tmp" && exec "$CLICKHOUSE_BIN" server -- --path="$tmp/data" \
      --http_port="$1" --tcp_port=0 --mysql_port=0 --postgresql_port=0 \
      --listen_host=127.0.0.1 >"$tmp/server.log" 2>&1 ) &
  srv_pid=$!
  for _ in $(seq 1 90); do
    if curl -s --max-time 2 "http://127.0.0.1:$1/ping" 2>/dev/null | grep -q Ok; then
      return 0
    fi
    kill -0 "$srv_pid" 2>/dev/null || return 1
    sleep 1
  done
  return 1
}

for p in $(seq "$PORT_BASE" $((PORT_BASE + 19))); do
  rm -rf "$tmp/data"
  if start_local "$p"; then PORTA="$p"; break; fi
  kill "$srv_pid" 2>/dev/null || true
  wait "$srv_pid" 2>/dev/null || true
  sleep 1
done
if [ -z "$PORTA" ]; then
  echo "nenhuma porta livre a partir de $PORT_BASE"; tail -20 "$tmp/server.log" 2>/dev/null || true; exit 1
fi

# ------------------------------------------------------------------ roundtrip
run_fixture() {
  env CLICKHOUSE_URL="$1" ${2:+CLICKHOUSE_USER="$2"} ${3:+CLICKHOUSE_PASSWORD="$3"} \
    "$BIN" executar "$FIXTURE"
}

check_output() {
  out="$1"
  fail=0
  # (a) INSERT refletido no SELECT de volta; Int64 -> inteiro, Float64 ->
  #     decimal (10.5 -> 10.5), Nullable(String) null -> nulo, Date -> texto
  echo "$out" | grep -q "linha: 1 ana 10.5 nulo 2024-03-01" || {
    echo "saida sem 'linha: 1 ana 10.5 nulo 2024-03-01': $out"; fail=1;
  }
  echo "$out" | grep -q "linha: 2 bruno 99.9 ok 2024-03-02" || {
    echo "saida sem 'linha: 2 bruno 99.9 ok 2024-03-02': $out"; fail=1;
  }
  # (b) erro de SQL invalido capturado com a mensagem do servidor
  echo "$out" | grep -q "erro-sql:.*Syntax error" || {
    echo "saida sem erro de SQL invalido capturado: $out"; fail=1;
  }
  # (c) tabela inexistente capturado com a mensagem do servidor
  echo "$out" | grep -q "erro-tabela:.*tabela_inexistente" || {
    echo "saida sem erro de tabela inexistente capturado: $out"; fail=1;
  }
  return "$fail"
}

# 1) URL com userinfo explicito
URL="clickhouse://default@127.0.0.1:$PORTA/default"
out=$(run_fixture "$URL")
check_output "$out" || exit 1

# 2) sem userinfo: autenticacao via env CLICKHOUSE_USER/CLICKHOUSE_PASSWORD
out2=$(run_fixture "clickhouse://127.0.0.1:$PORTA/default" default "")
echo "$out2" | grep -q "linha: 2 bruno 99.9 ok 2024-03-02" || {
  echo "saida da autenticacao por env sem 'linha: 2 bruno 99.9 ok 2024-03-02': $out2"; exit 1;
}

# 3) conferencia independente via HTTP: 2 linhas gravadas
cnt=$(curl -s -X POST --data "" \
  "http://127.0.0.1:$PORTA/?query=select%20count(*)%20from%20eventos&user=default")
echo "$cnt" | grep -q "^2" || {
  echo "esperado 2 linhas em eventos, obtido: $cnt"; exit 1;
}

echo "clickhouse_test ok"
exit 0
