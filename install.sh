#!/usr/bin/env bash
# Builds Rikugan for every host found on this machine and installs it:
#   Binary Ninja -> <BN user dir>/plugins/librikugan.so (.dylib)
#   IDA Pro      -> ~/.idapro/plugins/rikugan.so (.dylib)
#
#   ./install.sh                  # detect hosts in their default places
#   ./install.sh --binja-only | --ida-only
#   BN_INSTALL_DIR=... IDA_DIR=... OMC_DIR=... ./install.sh
#
# Needs: cmake, a C++20 compiler, OpenSSL 3 and zlib headers, curl + bsdtar or 7-Zip (to fetch the Qt headers that match each
# host: Qt 6.11.1 for Binary Ninja, 6.8.2 for IDA 9.3), and omc's source next to this repo (../ohmyc) or OMC_DIR.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")" && pwd)
cd "$ROOT"

WANT_BN=1
WANT_IDA=1
case "${1:-}" in
    --binja-only) WANT_IDA=0 ;;
    --ida-only) WANT_BN=0 ;;
    "") ;;
    *) echo "usage: $0 [--binja-only|--ida-only]"; exit 2 ;;
esac

OS=$(uname -s)
case "$OS-$(uname -m)" in
    Linux-aarch64) KIT=gcc_arm64 ;;
    Linux-x86_64)  KIT=gcc_64 ;;
    Darwin-*)      KIT=macos ;;
    *) echo "Unsupported platform: $OS-$(uname -m) (Rikugan builds on Linux and macOS)"; exit 1 ;;
esac
EXT=$([ "$OS" = Darwin ] && echo dylib || echo so)
OMC_DIR=${OMC_DIR:-$ROOT/../ohmyc}
GEN=$(command -v ninja >/dev/null && echo Ninja || echo "Unix Makefiles")

if [ ! -f "$OMC_DIR/include/omc.h" ]; then
    echo "omc source not found at $OMC_DIR."
    echo "Clone it next to this repository: git clone https://github.com/buzzer-re/Oh-My-C ../ohmyc"
    exit 1
fi
git submodule update --init --recursive third_party

qt() {  # qt <version> -> prints the kit dir, fetching it when missing
    local dir="$HOME/Qt/$1/$KIT"
    if [ ! -f "$dir/lib/cmake/Qt6/Qt6Config.cmake" ]; then
        tools/fetch_qt.sh "$1" "$HOME/Qt" >/dev/null || { echo "Could not fetch the Qt $1 headers (see above)." >&2; return 1; }
    fi
    echo "$dir"
}

first_dir() { for d in "$@"; do [ -d "$d" ] && { echo "$d"; return; }; done; }

installed=0

if [ "$WANT_BN" = 1 ]; then
    BN=${BN_INSTALL_DIR:-$(first_dir "$HOME/binaryninja" "/opt/binaryninja" "/Applications/Binary Ninja.app/Contents/MacOS")}
    if [ -n "$BN" ] && { [ -e "$BN/libbinaryninjacore.so.1" ] || [ -e "$BN/libbinaryninjacore.dylib" ]; }; then
        echo "== Binary Ninja at $BN"
        QT_BN=$(qt 6.11.1)
        cmake -S . -B build/binja -G "$GEN" -DRIKUGAN_HOST=binja -DOMC_DIR="$OMC_DIR" -DBN_INSTALL_DIR="$BN" \
            -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_PREFIX_PATH="$QT_BN"
        cmake --build build/binja --target rikugan_binja
        if [ "$OS" = Darwin ]; then DEST="$HOME/Library/Application Support/Binary Ninja/plugins"; else DEST="$HOME/.binaryninja/plugins"; fi
        mkdir -p "$DEST"
        cp "build/binja/librikugan.$EXT" "$DEST/"
        echo "Installed $DEST/librikugan.$EXT"
        installed=1
    else
        echo "== Binary Ninja: not found (set BN_INSTALL_DIR)"
    fi
fi

if [ "$WANT_IDA" = 1 ]; then
    IDA=${IDA_DIR:-$(first_dir "$HOME/ida-pro-9.3" "$HOME/idapro-9.3" "/opt/ida-pro-9.3" "/Applications/IDA Professional 9.3.app/Contents/MacOS")}
    if [ -n "$IDA" ] && { [ -e "$IDA/libida.so" ] || [ -e "$IDA/libida.dylib" ]; }; then
        echo "== IDA Pro at $IDA"
        QT_IDA=$(qt 6.8.2)
        cmake -S . -B build/ida -G "$GEN" -DRIKUGAN_HOST=ida -DOMC_DIR="$OMC_DIR" -DIDA_DIR="$IDA" \
            -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_PREFIX_PATH="$QT_IDA"
        cmake --build build/ida --target rikugan_ida
        DEST="$HOME/.idapro/plugins"
        mkdir -p "$DEST"
        cp "build/ida/rikugan.$EXT" "$DEST/"
        echo "Installed $DEST/rikugan.$EXT"
        installed=1
    else
        echo "== IDA Pro: not found (set IDA_DIR)"
    fi
fi

[ "$installed" = 1 ] || { echo "No host installed."; exit 1; }
echo "Done. Restart the host, then connect a provider from the model picker (Ctrl+M) or Settings → Provider."
