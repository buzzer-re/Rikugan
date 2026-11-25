#!/usr/bin/env bash
# Builds the Rikugan plugins for the hosts found on this machine and packages them as release assets in dist/:
#   dist/rikugan-<version>-<host>-<os>-<arch>.tar.gz (+ .sha256), host = ida | binja, os = linux | macos
# Each archive holds exactly what goes into the host's plugins folder (install.sh installs it; install.sh --from dist
# installs a local build). Windows: build.ps1.
#
#   ./build.sh                    # every host found
#   ./build.sh --sdk              # both hosts, without them installed (what CI and the release build)
#   ./build.sh --binja-only | --ida-only
#   BN_INSTALL_DIR=... IDA_DIR=... OMC_DIR=... ./build.sh
#
# --sdk links stand-ins for the hosts' libraries (cmake/host_stubs.cmake): the plugins bind to the host's own
# libraries when they load, like plugins built against an install.
#
# Needs: cmake, a C++20 compiler, OpenSSL 3 and zlib headers, curl + bsdtar or 7-Zip (to fetch the Qt headers that
# match each host: Qt 6.11.1 for Binary Ninja, 6.8.2 for IDA 9.3), python3 (--sdk), and omc's source next to this repo
# (../ohmyc) or OMC_DIR.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")" && pwd)
cd "$ROOT"

WANT_BN=1
WANT_IDA=1
SDK=0
for arg in "$@"; do
    case "$arg" in
        --binja-only) WANT_IDA=0 ;;
        --ida-only) WANT_BN=0 ;;
        --sdk) SDK=1 ;;
        *) echo "usage: $0 [--sdk] [--binja-only|--ida-only]"; exit 2 ;;
    esac
done

OS=$(uname -s)
case "$OS-$(uname -m)" in
    Linux-aarch64) KIT=gcc_arm64; PLAT=linux-arm64 ;;
    Linux-x86_64)  KIT=gcc_64;    PLAT=linux-x86_64 ;;
    Darwin-arm64)  KIT=macos;     PLAT=macos-arm64 ;;
    Darwin-x86_64) KIT=macos;     PLAT=macos-x86_64 ;;
    *) echo "Unsupported platform: $OS-$(uname -m) (Windows: build.ps1)"; exit 1 ;;
esac
EXT=$([ "$OS" = Darwin ] && echo dylib || echo so)
OMC_DIR=${OMC_DIR:-$ROOT/../ohmyc}
GEN=$(command -v ninja >/dev/null && echo Ninja || echo "Unix Makefiles")
VERSION=$(sed -n 's/^project(rikugan VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)

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

# archive <asset> <dir> <file...>: dist/<asset> (.tar.gz) holding the files of <dir>, and its .sha256
archive() {
    local asset=$1 dir=$2
    shift 2
    mkdir -p dist
    tar -czf "dist/$asset" -C "$dir" "$@"
    (cd dist && { sha256sum "$asset" 2>/dev/null || shasum -a 256 "$asset"; } > "$asset.sha256")
    echo "Packaged dist/$asset"
}

# package <host> <module>: dist/rikugan-<version>-<host>-<plat>.tar.gz holding the module without its debug
# information (what installs), and rikugan-<version>-<host>-<plat>-symbols.tar.gz with that information (to read a
# crash log's module+offset lines: Linux, a .debug file the module points to; macOS, a .dSYM bundle).
package() {
    local stage="build/package/$1" name
    name=$(basename "$2")
    rm -rf "$stage"
    mkdir -p "$stage"
    cp "$2" "$stage/$name"
    if [ "$OS" = Darwin ]; then
        dsymutil "$2" -o "$stage/$name.dSYM"
        strip -S "$stage/$name"  # the debug map only: crash stacks still name functions
        codesign --force --sign - "$stage/$name"  # stripping invalidates the linker's ad hoc signature (arm64 needs one)
        archive "rikugan-$VERSION-$1-$PLAT-symbols.tar.gz" "$stage" "$name.dSYM"
    else
        objcopy --only-keep-debug "$stage/$name" "$stage/$name.debug"
        strip --strip-debug "$stage/$name"
        (cd "$stage" && objcopy --add-gnu-debuglink="$name.debug" "$name")
        archive "rikugan-$VERSION-$1-$PLAT-symbols.tar.gz" "$stage" "$name.debug"
    fi
    archive "rikugan-$VERSION-$1-$PLAT.tar.gz" "$stage" "$name"
}

built=0

if [ "$WANT_BN" = 1 ] && [ "$SDK" = 1 ]; then
    echo "== Binary Ninja (no install: stand-ins for its libraries)"
    QT_BN=$(qt 6.11.1)
    cmake -S . -B build/binja-sdk -G "$GEN" -DRIKUGAN_HOST=binja -DRIKUGAN_SDK_ONLY=ON -DOMC_DIR="$OMC_DIR" \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_PREFIX_PATH="$QT_BN"
    cmake --build build/binja-sdk --target rikugan_binja
    package binja "build/binja-sdk/librikugan.$EXT"
    built=1
elif [ "$WANT_BN" = 1 ]; then
    BN=${BN_INSTALL_DIR:-$(first_dir "$HOME/binaryninja" "/opt/binaryninja" "/Applications/Binary Ninja.app/Contents/MacOS")}
    if [ -n "$BN" ] && { [ -e "$BN/libbinaryninjacore.so.1" ] || [ -e "$BN/libbinaryninjacore.dylib" ] ||
                         [ -e "$BN/libbinaryninjacore.1.dylib" ]; }; then
        echo "== Binary Ninja at $BN"
        QT_BN=$(qt 6.11.1)
        cmake -S . -B build/binja -G "$GEN" -DRIKUGAN_HOST=binja -DOMC_DIR="$OMC_DIR" -DBN_INSTALL_DIR="$BN" \
            -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_PREFIX_PATH="$QT_BN"
        cmake --build build/binja --target rikugan_binja
        package binja "build/binja/librikugan.$EXT"
        built=1
    else
        echo "== Binary Ninja: not found (set BN_INSTALL_DIR)"
    fi
fi

if [ "$WANT_IDA" = 1 ] && [ "$SDK" = 1 ]; then
    echo "== IDA Pro (no install: the SDK and stand-ins for IDA's Qt)"
    QT_IDA=$(qt 6.8.2)
    cmake -S . -B build/ida-sdk -G "$GEN" -DRIKUGAN_HOST=ida -DRIKUGAN_SDK_ONLY=ON -DOMC_DIR="$OMC_DIR" \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_PREFIX_PATH="$QT_IDA"
    cmake --build build/ida-sdk --target rikugan_ida
    package ida "build/ida-sdk/rikugan.$EXT"
    built=1
elif [ "$WANT_IDA" = 1 ]; then
    IDA=${IDA_DIR:-$(first_dir "$HOME/ida-pro-9.3" "$HOME/idapro-9.3" "/opt/ida-pro-9.3" "/Applications/IDA Professional 9.3.app/Contents/MacOS")}
    if [ -n "$IDA" ] && { [ -e "$IDA/libida.so" ] || [ -e "$IDA/libida.dylib" ]; }; then
        echo "== IDA Pro at $IDA"
        QT_IDA=$(qt 6.8.2)
        cmake -S . -B build/ida -G "$GEN" -DRIKUGAN_HOST=ida -DOMC_DIR="$OMC_DIR" -DIDA_DIR="$IDA" \
            -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_PREFIX_PATH="$QT_IDA"
        cmake --build build/ida --target rikugan_ida
        package ida "build/ida/rikugan.$EXT"
        built=1
    else
        echo "== IDA Pro: not found (set IDA_DIR)"
    fi
fi

[ "$built" = 1 ] || { echo "No host built."; exit 1; }
echo "Done. Install the packages with ./install.sh --from dist"
