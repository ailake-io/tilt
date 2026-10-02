#!/usr/bin/env sh
# `agenda:` cron com nomes (jan..dec, sun..sat), atalhos (@daily...) e a
# semantica classica OU entre dia-do-mes e dia-da-semana. Relogio fake:
# 2026-09-19 e um sabado, 08:00.
set -eu

BIN="$1"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fail=0

proxima() { # proxima "<cron>" -> "AAAA-MM-DD HH:MM" ou "ERRO"
  printf 'pipeline p:\n  agenda: "%s"\n  passos:\n    - imprimir "x"\n' "$1" >"$tmp/c.tilt"
  saida=$(TILT_AGORA="2026-09-19T08:00" TILT_AGENDAR_MAX=1 "$BIN" executar --agendar "$tmp/c.tilt" 2>&1 || true)
  case "$saida" in
    *"proxima execucao: "*) printf '%s' "$saida" | sed -n 's/^proxima execucao: //p' | head -1 ;;
    *) echo ERRO ;;
  esac
}
espera() { # espera "<cron>" "<esperado>"
  obtido=$(proxima "$1")
  [ "$obtido" = "$2" ] || { echo "FALHA: '$1' -> '$obtido' (esperado '$2')"; fail=1; }
}

espera "30 9 * * 1"        "2026-09-21 09:30"
espera "30 9 * * mon"      "2026-09-21 09:30"
espera "30 9 * * MON-FRI"  "2026-09-21 09:30"
espera "0 12 * jan *"      "2027-01-01 12:00"
espera "0 12 1 oct *"      "2026-10-01 12:00"
espera "@daily"            "2026-09-20 00:00"
espera "@hourly"           "2026-09-19 09:00"
espera "@weekly"           "2026-09-20 00:00"
espera "@monthly"          "2026-10-01 00:00"
espera "@yearly"           "2027-01-01 00:00"
# dia-do-mes e dia-da-semana restritos: OU (segunda 21 vem antes do dia 25)
espera "0 0 25 * mon"      "2026-09-21 00:00"
espera "0 0 25 * 1"        "2026-09-21 00:00"
# so dia-do-mes: E com o resto
espera "0 0 25 * *"        "2026-09-25 00:00"
espera "0 0 * * xyz"       "ERRO"
espera "0 0 * foo *"       "ERRO"

[ "$fail" = 0 ] && echo "cron_nomes_test ok"
exit "$fail"
