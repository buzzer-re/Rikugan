#!/usr/bin/env bash
# Local CI: builds the test tree and runs the native test suite, then builds each host plugin whose SDK/install is
# present. Mirrors .github/workflows/ci.yml.
#
#   ./ci-local.sh                 # everything available on this machine
#   OMC_DIR=../ohmyc QT_BN=~/Qt/6.11.1/gcc_arm64 QT_IDA=~/Qt/6.8.2/gcc_arm64 ./ci-local.sh
set -euo pipefail
ROOT=$(cd "$(dirname "$0")" && pwd)
cd "$ROOT"

case "$(uname -s)-$(uname -m)" in
    Linux-aarch64) KIT=gcc_arm64 ;;
    Linux-x86_64)  KIT=gcc_64 ;;
    Darwin-*)      KIT=macos ;;
esac
OMC_DIR=${OMC_DIR:-$ROOT/../ohmyc}
QT_BN=${QT_BN:-$HOME/Qt/6.11.1/$KIT}
QT_IDA=${QT_IDA:-$HOME/Qt/6.8.2/$KIT}
BN_INSTALL_DIR=${BN_INSTALL_DIR:-$HOME/binaryninja}
IDA_DIR=${IDA_DIR:-$HOME/ida-pro-9.3}
GEN=$(command -v ninja >/dev/null && echo Ninja || echo "Unix Makefiles")

step() { printf '\n== %s\n' "$*"; }

[ -f "$OMC_DIR/include/omc.h" ] || { echo "omc not found at $OMC_DIR (set OMC_DIR)"; exit 1; }
[ -x "$QT_BN/libexec/moc" ] || { echo "Qt not found at $QT_BN (tools/fetch_qt.sh 6.11.1)"; exit 1; }
git submodule update --init --recursive third_party >/dev/null

step "tests (Qt $(basename "$(dirname "$QT_BN")"))"
cmake -S . -B build/none -G "$GEN" -DRIKUGAN_HOST=none -DOMC_DIR="$OMC_DIR" -DCMAKE_PREFIX_PATH="$QT_BN" >/dev/null
cmake --build build/none
ctest --test-dir build/none --output-on-failure

if [ -e "$BN_INSTALL_DIR/libbinaryninjacore.so.1" ] || [ -e "$BN_INSTALL_DIR/libbinaryninjacore.dylib" ]; then
    step "Binary Ninja plugin"
    cmake -S . -B build/binja -G "$GEN" -DRIKUGAN_HOST=binja -DOMC_DIR="$OMC_DIR" -DBN_INSTALL_DIR="$BN_INSTALL_DIR" \
        -DCMAKE_PREFIX_PATH="$QT_BN" >/dev/null
    cmake --build build/binja --target rikugan_binja
else
    step "Binary Ninja plugin: skipped (no install at $BN_INSTALL_DIR)"
fi

if [ -e "$IDA_DIR/libida.so" ] || [ -e "$IDA_DIR/libida.dylib" ]; then
    if [ -x "$QT_IDA/libexec/moc" ]; then
        step "IDA plugin"
        cmake -S . -B build/ida -G "$GEN" -DRIKUGAN_HOST=ida -DOMC_DIR="$OMC_DIR" -DIDA_DIR="$IDA_DIR" \
            -DCMAKE_PREFIX_PATH="$QT_IDA" >/dev/null
        cmake --build build/ida
    else
        step "IDA plugin: skipped (Qt 6.8.2 headers not at $QT_IDA; tools/fetch_qt.sh 6.8.2)"
    fi
else
    step "IDA plugin: skipped (no install at $IDA_DIR)"
fi

step "OK"
