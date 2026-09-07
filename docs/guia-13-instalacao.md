# Guia 13 — Instalação e distribuição

Como instalar o `tilt` a partir do código-fonte, usar os binários
pré-compilados e gerar o instalador/pacote da linguagem (tarball, `.deb`,
Release do GitHub).

## 1. Requisitos

**Para compilar:**

| Requisito | Versão mínima |
|---|---|
| cmake | 3.20 |
| GCC ou Clang | GCC 11+ / Clang 14+ (C++20) |
| make/ninja | qualquer versão recente |

O `tilt` não tem dependências de link: SQLite, zlib (gzip no Parquet),
OpenSSL (TLS) e libpq (Postgres) são carregadas em runtime via `dlopen`
quando o recurso é usado.

**Em runtime:**

- `libc6` — única dependência obrigatória;
- `curl` — necessário apenas para chamadas HTTP externas reais (LLM, S3,
  Qdrant, Iceberg REST catalog). Instale com o gerenciador do sistema:
  `sudo apt install curl`.

## 2. Instalar do código-fonte

```bash
git clone https://github.com/ailake-io/tilt
cd tilt
sh scripts/install.sh                 # instala em ~/.local
```

Opções:

```bash
sh scripts/install.sh --prefix=/usr/local        # sistema (talvez com sudo)
sh scripts/install.sh --prefix="$HOME/opt/tilt"  # prefixo customizado
sh scripts/install.sh --uninstall                # remove o que foi instalado
```

O script configura um build Release em `build/install`, compila, instala
`bin/tilt` e `share/tilt/` (exemplos e documentos) sob o prefixo e imprime
a versão. Se o prefixo não estiver no `PATH`:

```bash
export PATH="$HOME/.local/bin:$PATH"
```

Para persistir, adicione a linha ao `~/.bashrc` (ou `~/.zshrc`).

## 3. Build manual (sem instalar)

Presets configurados (`CMakePresets.json`):

```bash
cmake --preset debug && cmake --build --preset debug      # dev: ASan+UBSan, -Werror
cmake --preset release && cmake --build --preset release  # -O3
ctest --preset release --output-on-failure                # suíte completa (sequencial)
```

O binário fica em `build/<preset>/bin/tilt`. O preset debug não é para
instalar — use o release (ou `scripts/install.sh`, que faz seu próprio
build Release).

## 4. Gerar o instalador (pacote)

O projeto usa **CPack** (já configurado no `CMakeLists.txt`). Gere o pacote
a partir do build de release:

```bash
cmake --preset release && cmake --build --preset release
cd build/release
cpack                 # tarball: tilt-<versao>-<os>-<arch>.tar.gz
cpack -G DEB          # pacote Debian: tilt-<versao>-<os>-<arch>.deb
```

**Tarball** (gerador padrão, `TGZ`) — contém `bin/tilt`,
`share/tilt/exemplos` e a documentação. Instalação manual:

```bash
tar xzf tilt-0.1.0-Linux-x86_64.tar.gz
sudo cp -r tilt-0.1.0-Linux-x86_64/{bin,share} /usr/local/
```

O binário Linux é portável: linka `libstdc++`/`libgcc` estaticamente
(opção `TILT_STATIC_LIBSTDCXX`, ligada por padrão), então roda em
distribuições antigas sem depender da versão do compilador do sistema.

**Pacote Debian/RPM** — `cpack` gera os dois de uma vez (o gerador TGZ é
sempre incluído):

```bash
cd build/release && cpack     # .tar.gz + .deb + .rpm de uma vez
```

- **Debian** (`tilt-<versao>-<os>-<arch>.deb`) — `Depends: libc6, curl`:
  ```bash
  sudo dpkg -i tilt-0.1.0-Linux-x86_64.deb
  sudo apt -f install        # se faltar alguma dependência
  sudo dpkg -r tilt          # desinstala
  ```
- **RPM** (`tilt-<versao>-<os>-<arch>.rpm`) — `Requires: glibc, curl`:
  ```bash
  sudo dnf install tilt-0.1.0-Linux-x86_64.rpm   # Fedora/RHEL
  sudo zypper install tilt-0.1.0-Linux-x86_64.rpm # openSUSE
  sudo rpm -e tilt                                 # desinstala
  ```

**Checksum** (para publicar o pacote):

```bash
sha256sum tilt-0.1.0-Linux-x86_64.tar.gz > tilt-0.1.0-Linux-x86_64.tar.gz.sha256
```

### Release no GitHub (automatizado)

O workflow `.github/workflows/release.yml` faz isso sozinho a cada tag `v*`:

```bash
git tag v0.1.0
git push origin v0.1.0
```

Ele compila em release, roda a suíte de testes e gera/anexa por
plataforma (Linux x86_64 e macOS arm64):

- `tilt-*.tar.gz` + `.sha256` (todas as plataformas);
- `tilt-*.deb` e `tilt-*.rpm` + `.sha256` (Linux, do CPack);
- `tilt-*.dmg` (macOS, do CPack DragNDrop);
- `tilt_*.snap` (job `snap`, via `snapcraft` em `snap/snapcraft.yaml` —
  publica na Snap Store automaticamente se o secret `SNAPCRAFT_TOKEN`
  estiver configurado, senão só anexa o arquivo);
- `tilt-*.vsix` (extensão VS Code).

O job `flatpak` valida o manifesto
(`packaging/flatpak/io.github.ailake_io.tilt.json`) com
`flatpak-builder`; para gerar o pacote localmente:

```bash
sudo apt install flatpak flatpak-builder
flatpak-builder --force-clean build-flatpak packaging/flatpak/io.github.ailake_io.tilt.json
```

O job `homebrew` valida a sintaxe da fórmula
(`packaging/homebrew/tilt.rb`). Para distribuir via Homebrew, crie um tap
(`ailake-io/homebrew-tilt`), copie a fórmula para `Formula/tilt.rb`,
preencha a `sha256` do tarball da Release e os usuários instalam com
`brew install ailake-io/tap/tilt`.

### Windows (.msi)

O empacotamento `.msi` (CPack WIX) está previsto, mas o runtime ainda é
POSIX (sockets BSD, epoll no servidor HTTP, dlopen). Antes do `.msi` é
precisa uma fase de portabilidade (camada de compat + backend
`select()`/IOCP no servidor HTTP). Ver `docs/guia-12-limitacoes.md`.

## 5. Extensão do VS Code

O instalador da extensão (`.vsix`) é gerado em `editors/vscode`:

```bash
cd editors/vscode
npm install
npm run package     # gera tilt-<versao>.vsix
```

Instale com `code --install-extension tilt-<versao>.vsix` ou pela aba
Extensões (opção "Install from VSIX"). O release workflow também anexa o
`.vsix` à Release e publica no Marketplace quando o secret `VSCE_PAT`
está configurado.

## 6. Verificar a instalação

```bash
tilt versao                 # imprime a versão
tilt referencia             # referência compacta da linguagem
tilt checar exemplos/soma.tilt   # valida um exemplo (se instalou os exemplos)
```

Um primeiro programa está no [README](../README.md#primeiro-programa).
Próximos passos: [guia 01 — Sintaxe](guia-01-sintaxe.md).
