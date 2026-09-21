#!/usr/bin/env sh
# `converter_fuso` com nomes IANA (tzdata do sistema): horario de verao, viagem de ida e
# volta e restauracao da variavel TZ. Pula (exit 0) sem tzdata ou no Windows.
set -eu

BIN="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
[ -f /usr/share/zoneinfo/America/Sao_Paulo ] || [ -n "${TZDIR:-}" ] || { echo "pulado: sem tzdata"; exit 0; }
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cat > "$tmp/p.tilt" <<'TILTEOF'
pipeline p:
  passos:
    - imprimir converter_fuso("2024-01-15T12:00:00", "UTC", "America/Sao_Paulo")
    - imprimir converter_fuso("2024-01-15T12:00:00", "UTC", "America/New_York")
    - imprimir converter_fuso("2024-07-15T12:00:00", "UTC", "America/New_York")
    - imprimir converter_fuso("2024-07-15T12:00:00", "Europe/London", "Asia/Tokyo")
    - imprimir converter_fuso("2024-03-10T09:00:00", "America/Sao_Paulo", "Europe/Lisbon")
    - v = converter_fuso("2024-05-05T08:15:00", "America/Sao_Paulo", "UTC")
    - imprimir converter_fuso(v, "UTC", "America/Sao_Paulo")
    - t = [{ quando: "2024-06-01T15:00:00" }]
    - r = t.converter_fuso "quando", origem: "UTC", destino: "America/Sao_Paulo"
    - imprimir r[0].quando
    - tentar:
        converter_fuso("2024-01-01", "../../etc/passwd", "UTC")
    capturar erro:
      imprimir "recusou nome invalido"
TILTEOF
out=$(TZ=Asia/Kolkata "$BIN" executar "$tmp/p.tilt" 2>&1) || { echo "FALHA: $out"; exit 1; }
esperado="== pipeline p ==
2024-01-15T09:00:00
2024-01-15T07:00:00
2024-07-15T08:00:00
2024-07-15T20:00:00
2024-03-10T12:00:00
2024-05-05T08:15:00
2024-06-01T12:00:00
recusou nome invalido"
[ "$out" = "$esperado" ] || { echo "FALHA: saida inesperada:"; echo "$out"; exit 1; }
echo "fuso ok"
