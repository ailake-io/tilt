#!/usr/bin/env sh
# Integration test do conector MySQL/MariaDB (libmariadb/libmysqlclient
# carregada via dlopen em runtime): roda fixtures/mysql_roundtrip.tilt contra
# um servidor real — criando a tabela via executar_sql "mysql://...", inserindo,
# atualizando, lendo de volta via fonte/ler (inteiro, decimal, tinyint como
# inteiro, nulo) e capturando os erros do servidor (PK duplicada e SQL
# invalido). O servidor sobe de uma de tres formas, na ordem:
#   1) mysqld local (MySQL >= 5.7): datadir temporario com --initialize-insecure
#   2) mariadbd local (MariaDB): datadir temporario com mariadb-install-db
#   3) docker: container mariadb:11 com senha de root temporaria
# Se nenhuma estiver disponivel, pula com mensagem (padrao dos outros testes).
# Em todos os casos o tilt tambem precisa achar a lib do cliente
# (libmariadb/libmysqlclient) — senao o runtime falha com erro acionavel.
set -eu

BIN="$1"
FIXTURE="${2:-${0%/*}/fixtures/mysql_roundtrip.tilt}"
PORT_BASE="${TILT_TEST_PORT:-8662}"
SENHA='tiltsenha'
ROOT_SENHA='tiltroot'

# ---------------------------------------------------------------- descoberta
# Acha servidor + cliente no PATH e em locais comuns (inclui layout conda:
# <prefix>/bin/mysqld com a lib em <prefix>/lib).
MYSQLD=""
MYSQL=""
MARIADB=""
MARIADB_CLIENT=""
for dir in $(echo "$PATH" | tr ':' ' ') /usr/sbin /usr/local/mysql/bin /usr/local/mariadb/bin /opt/mysql/bin; do
  [ -n "$dir" ] || continue
  if [ -z "$MYSQLD" ] && [ -x "$dir/mysqld" ]; then
    MYSQLD="$dir/mysqld"
    MYSQL="$dir/mysql"
  fi
  if [ -z "$MARIADB" ] && [ -x "$dir/mariadbd" ]; then
    MARIADB="$dir/mariadbd"
    if [ -x "$dir/mariadb" ]; then
      MARIADB_CLIENT="$dir/mariadb"
    else
      MARIADB_CLIENT="$dir/mysql"
    fi
  fi
done

DOCKER=""
if command -v docker >/dev/null 2>&1 && docker info >/dev/null 2>&1; then
  DOCKER=$(command -v docker)
fi

if [ -z "$MYSQLD" ] && [ -z "$MARIADB" ] && [ -z "$DOCKER" ]; then
  echo "mysqld/mariadbd/docker ausentes (instale MySQL/MariaDB ou docker); pulando o teste mysql"
  exit 0
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# Quando o servidor vem de um prefixo nao padrao (ex.: conda), a lib do
# cliente fica em <prefix>/lib — expoe para o dlopen do tilt (so quando a
# lib realmente esta la, para nao desviar a resolucao das demais libs).
for srv in "$MYSQLD" "$MARIADB"; do
  [ -n "$srv" ] || continue
  prefix_lib="$(dirname "$(dirname "$srv")")/lib"
  if ls "$prefix_lib"/libmariadb.so* "$prefix_lib"/libmysqlclient.so* >/dev/null 2>&1 \
     && [ "${LD_LIBRARY_PATH:-}" != *"$prefix_lib"* ]; then
    LD_LIBRARY_PATH="$prefix_lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    export LD_LIBRARY_PATH
  fi
done

PORTA=""
URL=""
MYSQL_BIN=""       # cliente SQL para setup/conferencia no servidor local
setup_sql() { :; } # cria banco + usuario no servidor local
count_sql() { :; } # conta as linhas gravadas (conferencia independente)
stop_sql() { :; }  # derruba o servidor local

# ---------------------------------------------------------- servidor local
if [ -n "$MYSQLD" ] || [ -n "$MARIADB" ]; then
  data="$tmp/data"
  sock="$tmp/mysql.sock"
  mkdir -p "$data"
  if [ -n "$MYSQLD" ]; then
    SERVER="$MYSQLD"
    MYSQL_BIN="$MYSQL"
    IS_MARIADB=0
    # MySQL 8.4+ desabilita mysql_native_password por padrao: religa quando a
    # versao suporta a opcao. X plugin desligado para nao disputar a porta
    # 33060 com outros servidores de teste.
    extra=""
    if "$MYSQLD" --no-defaults --verbose --help 2>/dev/null | grep -q -- "--mysql-native-password"; then
      extra="$extra --mysql-native-password=ON"
    fi
    if "$MYSQLD" --no-defaults --verbose --help 2>/dev/null | grep -q -- "--mysqlx"; then
      extra="$extra --mysqlx=OFF"
    fi
    # --no-defaults ignora o my.cnf do sistema (no runner ubuntu o pacote
    # aponta log_error para /var/log/mysql, sem permissao para o usuario).
    "$MYSQLD" --no-defaults --initialize-insecure --datadir="$data" \
      --log-error="$tmp/init-error.log" >"$tmp/init.log" 2>&1 || {
      echo "mysqld --initialize-insecure falhou:"; tail -20 "$tmp/init.log"; exit 1;
    }
  else
    SERVER="$MARIADB"
    MYSQL_BIN="$MARIADB_CLIENT"
    IS_MARIADB=1
    extra=""
    install_db=""
    for c in "$(dirname "$MARIADB")/mariadb-install-db" "$(dirname "$MARIADB")/mysql_install_db"; do
      if [ -x "$c" ]; then install_db="$c"; break; fi
    done
    if [ -z "$install_db" ]; then
      echo "mariadb-install-db ausente; pulando o teste mysql"
      exit 0
    fi
    "$install_db" --defaults-file=/dev/null --datadir="$data" \
      --auth-root-authentication-method=normal \
      >"$tmp/init.log" 2>&1 || {
      echo "mariadb-install-db falhou:"; tail -20 "$tmp/init.log"; exit 1;
    }
  fi
  if [ ! -x "$MYSQL_BIN" ]; then
    echo "cliente mysql/mariadb ausente; pulando o teste mysql"
    exit 0
  fi

  stop_sql() {
    "$MYSQL_BIN" --socket="$sock" -uroot -e "shutdown" >/dev/null 2>&1 || true
    if [ -f "$tmp/mysqld.pid" ]; then
      kill "$(cat "$tmp/mysqld.pid")" >/dev/null 2>&1 || true
    fi
  }
  trap 'stop_sql; rm -rf "$tmp"' EXIT

  start_local() {
    # shellcheck disable=SC2086
    "$SERVER" --no-defaults --datadir="$data" --port="$1" --socket="$sock" \
      --skip-networking=0 --bind-address=127.0.0.1 \
      --pid-file="$tmp/mysqld.pid" --log-error="$tmp/server.log" $extra &
    srv_pid=$!
    for _ in $(seq 1 60); do
      if "$MYSQL_BIN" --socket="$sock" -uroot -e "select 1" >/dev/null 2>&1; then
        return 0
      fi
      kill -0 "$srv_pid" 2>/dev/null || return 1
      sleep 1
    done
    return 1
  }

  for p in $(seq "$PORT_BASE" $((PORT_BASE + 19))); do
    if start_local "$p"; then PORTA="$p"; break; fi
    stop_sql
    sleep 1
  done
  if [ -z "$PORTA" ]; then
    echo "nenhuma porta livre a partir de $PORT_BASE"; tail -20 "$tmp/server.log"; exit 1
  fi

  if [ "$IS_MARIADB" = 1 ]; then
    setup_sql() {
      "$MYSQL_BIN" --socket="$sock" -uroot -e \
        "create database if not exists tilt_test; \
         create user if not exists 'tilt'@'%' identified by '$SENHA'; \
         grant all on tilt_test.* to 'tilt'@'%'; flush privileges;"
    }
  else
    # usuario com mysql_native_password: roda sobre TCP sem TLS com qualquer
    # lib cliente 8.0 (caching_sha2 exigiria canal seguro ou chave publica).
    setup_sql() {
      "$MYSQL_BIN" --socket="$sock" -uroot -e \
        "create database if not exists tilt_test; \
         create user if not exists 'tilt'@'%' identified with mysql_native_password by '$SENHA'; \
         grant all on tilt_test.* to 'tilt'@'%'; flush privileges;"
    }
  fi
  count_sql() {
    "$MYSQL_BIN" --socket="$sock" -uroot -N -B -e "select count(*) from tilt_test.clientes"
  }

  setup_sql
  URL="mysql://tilt:$SENHA@127.0.0.1:$PORTA/tilt_test"

# ---------------------------------------------------------- docker fallback
elif [ -n "$DOCKER" ]; then
  name="tilt-mysql-test-$$"
  for p in $(seq "$PORT_BASE" $((PORT_BASE + 19))); do
    if "$DOCKER" run --rm -d --name "$name" \
        -e MARIADB_ROOT_PASSWORD="$ROOT_SENHA" \
        -e MARIADB_DATABASE=tilt_test \
        -p "127.0.0.1:$p:3306" mariadb:11 >"$tmp/docker.log" 2>&1; then
      PORTA="$p"; break
    fi
    "$DOCKER" rm -f "$name" >/dev/null 2>&1 || true
    sleep 1
  done
  if [ -z "$PORTA" ]; then
    echo "docker run mariadb:11 falhou:"; cat "$tmp/docker.log"; exit 1
  fi
  trap '"$DOCKER" rm -f "$name" >/dev/null 2>&1; rm -rf "$tmp"' EXIT

  root_sql() { "$DOCKER" exec "$name" mariadb -uroot -p"$ROOT_SENHA" -e "$1"; }
  ping_docker() { "$DOCKER" exec "$name" mariadb-admin -uroot -p"$ROOT_SENHA" ping >/dev/null 2>&1; }
  for _ in $(seq 1 90); do
    if ping_docker; then break; fi
    sleep 1
  done
  if ! ping_docker; then
    echo "mariadb:11 nao respondeu a tempo:"; "$DOCKER" logs "$name" | tail -20; exit 1
  fi
  setup_sql() { root_sql "select 1"; }
  count_sql() { root_sql "select count(*) from tilt_test.clientes"; }
  URL="mysql://root:$ROOT_SENHA@127.0.0.1:$PORTA/tilt_test"
fi

# ---------------------------------------------------------------- roundtrip
run_fixture() {
  env MYSQL_URL="$1" "$BIN" executar "$FIXTURE"
}

check_output() {
  out="$1"
  fail=0
  # (a) INSERT + UPDATE refletidos no SELECT de volta; decimal 10.50 -> 10.5,
  #     tinyint vira inteiro (1/0) e NULL vira nulo
  echo "$out" | grep -q "linha: 1 ana 10.5 1 nulo" || {
    echo "saida sem 'linha: 1 ana 10.5 1 nulo': $out"; fail=1;
  }
  echo "$out" | grep -q "linha: 2 bruno 99.9 0 ok" || {
    echo "saida sem 'linha: 2 bruno 99.9 0 ok': $out"; fail=1;
  }
  # (b) erro de constraint do servidor capturado com a mensagem do servidor
  echo "$out" | grep -q "erro-constraint:.*Duplicate entry" || {
    echo "saida sem erro de constraint capturado: $out"; fail=1;
  }
  # (c) SQL invalido capturado com a mensagem do parser
  echo "$out" | grep -q "erro-sql:.*syntax" || {
    echo "saida sem erro de SQL invalido capturado: $out"; fail=1;
  }
  return "$fail"
}

# 1) esquema mysql://
out=$(run_fixture "$URL")
check_output "$out" || exit 1

# 2) o esquema alternativo mariadb:// aceita o mesmo fixture (a lib C e a
#    mesma; o nome do esquema so seleciona o conector)
out2=$(run_fixture "$(echo "$URL" | sed 's|^mysql://|mariadb://|')")
echo "$out2" | grep -q "linha: 2 bruno 99.9 0 ok" || {
  echo "saida do esquema mariadb:// sem 'linha: 2 bruno 99.9 0 ok': $out2"; exit 1;
}

# 3) conferencia independente com o cliente SQL: 2 linhas gravadas
cnt=$(count_sql)
echo "$cnt" | grep -q "^2" || { echo "esperado 2 linhas em clientes, obtido: $cnt"; exit 1; }

echo "mysql_test ok"
exit 0
