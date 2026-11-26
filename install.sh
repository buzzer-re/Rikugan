#!/usr/bin/env bash
# Installs a Rikugan release for every host found on this machine (Linux and macOS; Windows: install.ps1):
#   Binary Ninja -> <BN user dir>/plugins/librikugan.so (.dylib)
#   IDA Pro      -> ~/.idapro/plugins/rikugan.so (.dylib)
# The packages come from the GitHub release (rikugan-<version>-<host>-<os>-<arch>.tar.gz, checked against its .sha256),
# or from a local folder of packages built with ./build.sh (--from dist).
#
#   curl -fsSL https://raw.githubusercontent.com/buzzer-re/Rikugan/main/install.sh | bash
#   ./install.sh [--binja-only | --ida-only] [--version X.Y.Z] [--from DIR] [--yes]
#
# Environment: RIKUGAN_VERSION, RIKUGAN_REPO (owner/name), RIKUGAN_DOWNLOAD_URL (release download base), BN_USER_DIR,
# IDA_USER_DIR. A Rikugan 1.x (Python) install of the same host is moved to ~/.rikugan-1.x-backup/ first (asks).
set -euo pipefail

WANT_BN=1
WANT_IDA=1
VERSION=${RIKUGAN_VERSION:-}
FROM=""
YES=0
while [ $# -gt 0 ]; do
    case "$1" in
        --binja-only) WANT_IDA=0 ;;
        --ida-only) WANT_BN=0 ;;
        --version) VERSION=${2:?--version needs a value}; shift ;;
        --from) FROM=${2:?--from needs a folder}; shift ;;
        --yes|-y) YES=1 ;;
        *) echo "usage: $0 [--binja-only|--ida-only] [--version X.Y.Z] [--from DIR] [--yes]"; exit 2 ;;
    esac
    shift
done

REPO=${RIKUGAN_REPO:-buzzer-re/Rikugan}
BASE=${RIKUGAN_DOWNLOAD_URL:-https://github.com/$REPO/releases/download}
OS=$(uname -s)
case "$OS-$(uname -m)" in
    Linux-aarch64) PLAT=linux-arm64 ;;
    Linux-x86_64)  PLAT=linux-x86_64 ;;
    Darwin-arm64)  PLAT=macos-arm64 ;;
    Darwin-x86_64) PLAT=macos-x86_64 ;;
    *) echo "Unsupported platform: $OS-$(uname -m) (Windows: install.ps1)"; exit 1 ;;
esac
EXT=$([ "$OS" = Darwin ] && echo dylib || echo so)

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

ask() {  # ask <question>: true unless the user says no (always with --yes; never without a terminal)
    [ "$YES" = 1 ] && return 0
    [ -r /dev/tty ] || return 1
    local a
    read -r -p "$1 [Y/n] " a </dev/tty || return 1
    case "$a" in n|N|no|NO|No) return 1 ;; *) return 0 ;; esac
}

# fetch <host>: prints the path of the verified package for this platform
fetch() {
    local asset
    if [ -n "$FROM" ]; then
        asset=$(cd "$FROM" && ls -1 rikugan-*-"$1"-"$PLAT".tar.gz 2>/dev/null | sort -V | tail -n 1)
        [ -n "$asset" ] || { echo "no rikugan-*-$1-$PLAT.tar.gz in $FROM (./build.sh makes them)" >&2; return 1; }
        cp "$FROM/$asset" "$FROM/$asset.sha256" "$TMP/" 2>/dev/null || cp "$FROM/$asset" "$TMP/"
    else
        if [ -z "$VERSION" ]; then
            VERSION=$(curl -fsSL "https://api.github.com/repos/$REPO/releases/latest" | sed -n 's/.*"tag_name": *"v\{0,1\}\([^"]*\)".*/\1/p' | head -n 1)
            [ -n "$VERSION" ] || { echo "could not find the latest release of $REPO" >&2; return 1; }
        fi
        VERSION=${VERSION#v}
        asset="rikugan-$VERSION-$1-$PLAT.tar.gz"
        curl -fsSL -o "$TMP/$asset" "$BASE/v$VERSION/$asset" || { echo "download failed: $BASE/v$VERSION/$asset" >&2; return 1; }
        curl -fsSL -o "$TMP/$asset.sha256" "$BASE/v$VERSION/$asset.sha256" || { echo "download failed: $asset.sha256" >&2; return 1; }
    fi
    if [ -f "$TMP/$asset.sha256" ]; then
        local want got
        want=$(awk '{print $1}' "$TMP/$asset.sha256")
        got=$( (sha256sum "$TMP/$asset" 2>/dev/null || shasum -a 256 "$TMP/$asset") | awk '{print $1}')
        [ "$want" = "$got" ] || { echo "checksum mismatch for $asset; nothing was installed" >&2; return 1; }
    elif [ -z "$FROM" ]; then
        echo "no checksum for $asset" >&2; return 1
    fi
    echo "$TMP/$asset"
}

# move_aside <path> <host>: a Rikugan 1.x file or folder goes to ~/.rikugan-1.x-backup/<host>/<time>/
move_aside() {
    local dest
    dest="$HOME/.rikugan-1.x-backup/$2/$(date +%Y%m%d-%H%M%S)"
    mkdir -p "$dest"
    mv "$1" "$dest/"
    echo "Moved $1 to $dest"
}

installed=0

if [ "$WANT_BN" = 1 ]; then
    if [ "$OS" = Darwin ]; then BN_USER=${BN_USER_DIR:-$HOME/Library/Application Support/Binary Ninja}
    else BN_USER=${BN_USER_DIR:-$HOME/.binaryninja}; fi
    if [ -d "$BN_USER" ]; then
        echo "== Binary Ninja ($BN_USER)"
        pkg=$(fetch binja)
        dest="$BN_USER/plugins"
        mkdir -p "$dest"
        if [ -e "$dest/rikugan" ]; then
            ask "$dest/rikugan holds Rikugan 1.x (or a checkout), which would load next to 2.x. Move it to ~/.rikugan-1.x-backup?" ||
                { echo "Not installed: $dest/rikugan is in the way."; exit 1; }
            move_aside "$dest/rikugan" binaryninja
        fi
        tar -xzf "$pkg" -C "$dest"
        echo "Installed $dest/librikugan.$EXT"
        installed=1
    else
        echo "== Binary Ninja: no user folder at $BN_USER (run Binary Ninja once, or set BN_USER_DIR)"
    fi
fi

if [ "$WANT_IDA" = 1 ]; then
    IDA_USER=${IDA_USER_DIR:-${IDAUSR:-$HOME/.idapro}}
    IDA_USER=${IDA_USER%%:*}
    if [ -d "$IDA_USER" ]; then
        echo "== IDA Pro ($IDA_USER)"
        pkg=$(fetch ida)
        dest="$IDA_USER/plugins"
        mkdir -p "$dest"
        if [ -e "$dest/rikugan_plugin.py" ] || [ -e "$dest/rikugan" ]; then
            ask "IDA has Rikugan 1.x (rikugan_plugin.py / plugins/rikugan), which would load next to 2.x. Move it to ~/.rikugan-1.x-backup?" ||
                { echo "Not installed: Rikugan 1.x is in the way."; exit 1; }
            [ -e "$dest/rikugan_plugin.py" ] && move_aside "$dest/rikugan_plugin.py" ida
            [ -e "$dest/rikugan" ] && move_aside "$dest/rikugan" ida
        fi
        tar -xzf "$pkg" -C "$dest"
        echo "Installed $dest/rikugan.$EXT"
        installed=1
    else
        echo "== IDA Pro: no user folder at $IDA_USER (run IDA once, or set IDA_USER_DIR)"
    fi
fi

[ "$installed" = 1 ] || { echo "No host installed."; exit 1; }
echo "Done. Restart the host, then connect a provider from the model picker (Ctrl+M) or Settings → Provider."
echo "Rikugan installs later releases itself, when you agree (Settings → Behavior → Updates)."
