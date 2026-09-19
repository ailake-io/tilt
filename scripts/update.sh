#!/usr/bin/env sh
# Atualiza uma instalação POSIX do tilt a partir do GitHub Release.
# Uso: tilt-atualizar [--check] [--force] [--prefix=DIR] [--repo=ORG/REPO]
set -eu

PREFIX="${TILT_UPDATE_PREFIX:-$HOME/.local}"
REPO="${TILT_UPDATE_REPO:-ailake-io/tilt}"
API_URL=""
PINNED_TAG=""
CHECK_ONLY=0
FORCE=0

for arg in "$@"; do
  case "$arg" in
    --prefix=*) PREFIX="${arg#--prefix=}" ;;
    --repo=*) REPO="${arg#--repo=}" ;;
    --api-url=*) API_URL="${arg#--api-url=}" ;;
    --version=*) PINNED_TAG="${arg#--version=}" ;;
    --check) CHECK_ONLY=1 ;;
    --force) FORCE=1 ;;
    -h|--help)
      echo "uso: $0 [--check] [--force] [--prefix=DIR] [--repo=ORG/REPO] [--version=vX.Y.Z]"
      exit 0
      ;;
    *) echo "opcao desconhecida: $arg" >&2; exit 2 ;;
  esac
done

command -v curl >/dev/null 2>&1 || { echo "erro: curl nao encontrado" >&2; exit 1; }
command -v python3 >/dev/null 2>&1 || { echo "erro: python3 e necessario para ler o manifesto GitHub" >&2; exit 1; }
command -v tar >/dev/null 2>&1 || { echo "erro: tar nao encontrado" >&2; exit 1; }

case "$(uname -s)" in
  Linux) OS=Linux ;;
  Darwin) OS=macos ;;
  *) echo "erro: plataforma nao suportada pelo atualizador: $(uname -s)" >&2; exit 1 ;;
esac
case "$(uname -m)" in
  x86_64|amd64) ARCH=x86_64 ;;
  aarch64|arm64) ARCH=arm64 ;;
  *) echo "erro: arquitetura nao suportada pelo atualizador: $(uname -m)" >&2; exit 1 ;;
esac

if [ -z "$API_URL" ]; then
  if [ -n "$PINNED_TAG" ]; then
    API_URL="https://api.github.com/repos/$REPO/releases/tags/$PINNED_TAG"
  else
    API_URL="https://api.github.com/repos/$REPO/releases/latest"
  fi
fi

TMP=$(mktemp -d "${TMPDIR:-/tmp}/tilt-update.XXXXXX")
trap 'rm -rf "$TMP"' EXIT
META="$TMP/release.json"
curl -fsSL -H 'Accept: application/vnd.github+json' -H 'User-Agent: tilt-updater' \
  -o "$META" "$API_URL"

TAG=$(python3 - "$META" <<'PY'
import json, sys
with open(sys.argv[1], encoding="utf-8") as f:
    data = json.load(f)
tag = data.get("tag_name", "")
if not isinstance(tag, str) or not tag.startswith("v") or len(tag) < 2:
    raise SystemExit("manifesto sem tag_name vX.Y.Z")
print(tag)
PY
)
VERSION=${TAG#v}
ASSET="tilt-${VERSION}-${OS}-${ARCH}.tar.gz"

ASSET_URL=$(python3 - "$META" "$ASSET" <<'PY'
import json, sys
with open(sys.argv[1], encoding="utf-8") as f:
    data = json.load(f)
name = sys.argv[2]
for asset in data.get("assets", []):
    if asset.get("name") == name:
        print(asset.get("browser_download_url", ""))
        break
else:
    raise SystemExit("artefato nao encontrado: " + name)
PY
)
CHECK_URL=$(python3 - "$META" "$ASSET.sha256" <<'PY'
import json, sys
with open(sys.argv[1], encoding="utf-8") as f:
    data = json.load(f)
name = sys.argv[2]
for asset in data.get("assets", []):
    if asset.get("name") == name:
        print(asset.get("browser_download_url", ""))
        break
else:
    raise SystemExit("checksum nao encontrado: " + name)
PY
)

CURRENT="desconhecida"
if [ -x "$PREFIX/bin/tilt" ]; then
  CURRENT=$("$PREFIX/bin/tilt" versao 2>/dev/null | awk 'NR == 1 { print $2; exit }' || true)
fi
if [ "$CURRENT" = "$VERSION" ] && [ "$FORCE" -eq 0 ]; then
  echo "tilt $CURRENT ja esta atualizado"
  exit 0
fi
if [ "$CHECK_ONLY" -eq 1 ]; then
  echo "atualizacao disponivel: tilt ${CURRENT} -> tilt ${VERSION}"
  exit 0
fi

echo "baixando tilt ${VERSION} (${OS}/${ARCH})..."
ARCHIVE="$TMP/$ASSET"
CHECKSUM="$TMP/$ASSET.sha256"
curl -fsSL -H 'User-Agent: tilt-updater' -o "$ARCHIVE" "$ASSET_URL"
curl -fsSL -H 'User-Agent: tilt-updater' -o "$CHECKSUM" "$CHECK_URL"
EXPECTED=$(awk 'NF { print $1; exit }' "$CHECKSUM")
case "$EXPECTED" in
  ''|*[!0123456789abcdefABCDEF]*) echo "erro: checksum invalido" >&2; exit 1 ;;
esac
if command -v sha256sum >/dev/null 2>&1; then
  ACTUAL=$(sha256sum "$ARCHIVE" | awk '{print $1}')
else
  ACTUAL=$(shasum -a 256 "$ARCHIVE" | awk '{print $1}')
fi
[ "$ACTUAL" = "$EXPECTED" ] || {
  echo "erro: checksum do artefato nao confere" >&2
  exit 1
}

STAGE="$TMP/stage"
mkdir -p "$STAGE"
tar -xzf "$ARCHIVE" -C "$STAGE"
[ -f "$STAGE/bin/tilt" ] || { echo "erro: pacote sem bin/tilt" >&2; exit 1; }
[ -d "$STAGE/share/tilt" ] || { echo "erro: pacote sem share/tilt" >&2; exit 1; }

mkdir -p "$PREFIX/bin" "$PREFIX/share"
NEW_BIN="$PREFIX/bin/.tilt.new.$$"
NEW_SHARE="$PREFIX/share/.tilt.new.$$"
OLD_SHARE="$PREFIX/share/.tilt.previous.$$"
cleanup_install() {
  rm -f "$NEW_BIN"
  rm -rf "$NEW_SHARE"
}
trap 'cleanup_install; rm -rf "$TMP"' EXIT
install -m 0755 "$STAGE/bin/tilt" "$NEW_BIN"
mkdir "$NEW_SHARE"
cp -R "$STAGE/share/tilt/." "$NEW_SHARE/"
mv "$NEW_BIN" "$PREFIX/bin/tilt"
if [ -e "$PREFIX/share/tilt" ]; then mv "$PREFIX/share/tilt" "$OLD_SHARE"; fi
if ! mv "$NEW_SHARE" "$PREFIX/share/tilt"; then
  rm -rf "$PREFIX/share/tilt"
  if [ -e "$OLD_SHARE" ]; then mv "$OLD_SHARE" "$PREFIX/share/tilt"; fi
  echo "erro: nao foi possivel ativar a stdlib nova" >&2
  exit 1
fi
rm -rf "$OLD_SHARE"
trap 'rm -rf "$TMP"' EXIT

echo "tilt atualizado: $CURRENT -> $VERSION"
"$PREFIX/bin/tilt" versao
