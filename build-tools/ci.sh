#!/usr/bin/env bash
# CI and release steps, the same on a GitHub runner and locally (.github/workflows/*.yml call this).
#
#   build-tools/ci.sh deps           what the build needs is present (installs nothing, by design)
#   build-tools/ci.sh omc            omc's source at the commit Rikugan builds against (build-tools/omc.ref), in $OMC_DIR
#   build-tools/ci.sh libomc         Windows (Git Bash): libomc.dll from that source, with the runner's MinGW-w64 gcc
#   build-tools/ci.sh test           the native test suite (build/none): zero warnings, then ctest
#   build-tools/ci.sh plugins        both plugins without the hosts installed (build.sh --sdk): zero warnings, packaged
#                                    into dist/ as the release ships them
#   build-tools/ci.sh smoke          each package in dist/: what its plugin links and exports, checked as it ships
#   build-tools/ci.sh tag-check T    the release tag T is v<version> of CMakeLists.txt and ida-plugin.json
#   build-tools/ci.sh dist-assets    the files `plugins` made, for the release upload
#
# Windows builds with MSVC: build-tools/ci.ps1 runs the test, plugins and smoke steps there.
set -euo pipefail
cd "$(dirname "$0")/.."

OS=$(uname -s)
ARCH=$(uname -m)
VERSION=$(sed -n 's/^project(rikugan VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
export OMC_DIR=${OMC_DIR:-$PWD/build/ohmyc}
QT_ROOT=${QT_ROOT:-$HOME/Qt}
GEN=$(command -v ninja >/dev/null && echo Ninja || echo "Unix Makefiles")
case "$OS-$ARCH" in
    Linux-aarch64) KIT=gcc_arm64 ;;
    Linux-x86_64)  KIT=gcc_64 ;;
    Darwin-*)      KIT=macos ;;
    *)             KIT= ;;
esac

fail() {
    echo "ci: $*" >&2
    exit 1
}

# zero_warnings <log>: the build output in <log> has no compiler or linker warning
zero_warnings() {
    local n
    n=$(grep -ciE '(^|[ :])warning( [A-Z]+[0-9]+)?:' "$1" || true)
    [ "$n" = 0 ] || { grep -iE -A3 '(^|[ :])warning( [A-Z]+[0-9]+)?:' "$1" | head -60; fail "$n warning(s) in the build; it must have none"; }
}

deps() {
    local t
    for t in cmake git curl make python3 c++; do command -v "$t" >/dev/null || fail "no $t"; done
    command -v bsdtar >/dev/null || tar --version 2>/dev/null | grep -q bsdtar || command -v 7z >/dev/null ||
        fail "nothing reads Qt's .7z archives (bsdtar: Linux libarchive-tools; or 7-Zip)"
    if [ "$OS" = Darwin ]; then
        local p
        p=$(brew --prefix openssl@3 2>/dev/null || true)
        [ -n "$p" ] && [ -f "$p/lib/libssl.a" ] && [ -f "$p/lib/libcrypto.a" ] ||
            fail "Homebrew OpenSSL 3 with its static libraries not found: the plugins link it in"
        command -v c++filt >/dev/null || fail "no c++filt (the Xcode command line tools)"
        echo "ci: OpenSSL from $p, macOS $(sw_vers -productVersion)"
    else
        printf '#include <openssl/ssl.h>\n#include <zlib.h>\nint main(void){return 0;}\n' |
            cc -x c - -o /dev/null -lssl -lcrypto -lz 2>/dev/null || fail "OpenSSL or zlib development files missing"
        command -v c++filt >/dev/null || fail "no c++filt (binutils)"
        echo "ci: OpenSSL $(openssl version | cut -d' ' -f2), glibc $(ldd --version | head -1 | awk '{print $NF}')"
    fi
    echo "ci: Rikugan $VERSION, $(cmake --version | head -1), $(c++ --version | head -1), $GEN"
}

omc() {
    local ref
    ref=$(grep -v '^#' build-tools/omc.ref | awk 'NF {print $1; exit}')
    [ -n "$ref" ] || fail "build-tools/omc.ref names no commit"
    if [ ! -d "$OMC_DIR/.git" ]; then
        mkdir -p "$OMC_DIR"
        git -C "$OMC_DIR" init -q
        git -C "$OMC_DIR" remote add origin https://github.com/buzzer-re/Oh-My-C.git
    fi
    git -C "$OMC_DIR" fetch -q --depth=1 origin "$ref"
    git -C "$OMC_DIR" -c advice.detachedHead=false checkout -q FETCH_HEAD
    echo "ci: omc $(git -C "$OMC_DIR" rev-parse --short HEAD) in $OMC_DIR (OMC_API_VERSION $(sed -n 's/^#define OMC_API_VERSION //p' "$OMC_DIR/include/omc.h"))"
}

libomc() {
    case "$OS" in MINGW* | MSYS* | CYGWIN*) ;; *) fail "libomc is the Windows step (Git Bash)" ;; esac
    local jobs py
    jobs=$(nproc 2>/dev/null || echo 4)
    py=$(command -v python3 || command -v python)
    make -s -j"$jobs" -C "$OMC_DIR" WINDOWS=1 MINGW= HOSTCC=gcc PYTHON="$py" build/windows/libomc.dll
    for f in libomc.dll libomc.def; do [ -f "$OMC_DIR/build/windows/$f" ] || fail "no $OMC_DIR/build/windows/$f"; done
    echo "ci: $(ls -l "$OMC_DIR/build/windows/libomc.dll")"
}

qt_kit() {  # qt_kit <version>: the kit folder, fetched when missing
    local dir="$QT_ROOT/$1/$KIT"
    [ -f "$dir/lib/cmake/Qt6/Qt6Config.cmake" ] || tools/fetch_qt.sh "$1" "$QT_ROOT" >&2 || fail "cannot fetch Qt $1"
    echo "$dir"
}

test_suite() {
    local qt
    qt=$(qt_kit 6.11.1)
    mkdir -p build
    cmake -S . -B build/none -G "$GEN" -DRIKUGAN_HOST=none -DOMC_DIR="$OMC_DIR" -DCMAKE_PREFIX_PATH="$qt"
    cmake --build build/none 2>&1 | tee build/ci-test-build.log
    [ "${PIPESTATUS[0]}" = 0 ] || fail "the test build failed"
    zero_warnings build/ci-test-build.log
    ctest --test-dir build/none --output-on-failure
}

plugins() {
    qt_kit 6.11.1 >/dev/null
    qt_kit 6.8.2 >/dev/null
    rm -rf dist
    mkdir -p build
    QT_ROOT="$QT_ROOT" ./build.sh --sdk 2>&1 | tee build/ci-plugins.log
    [ "${PIPESTATUS[0]}" = 0 ] || fail "the plugin build failed"
    zero_warnings build/ci-plugins.log
    ls -l dist
}

# check_linux <module> <host>: what an ELF plugin needs from the system and the host, and what it exports
check_linux() {
    local needed exports glibc
    needed=$(readelf -d "$1" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p')
    echo "$needed" | sed 's/^/  needs /'
    echo "$needed" | grep -vqE '^(libbinaryninjacore\.so\.1|libbinaryninjaui\.so\.1|libQt6(Core|Gui|Widgets)\.so\.6|libida\.so|libssl\.so\.3|libcrypto\.so\.3|libz\.so\.1|libstdc\+\+\.so\.6|libm\.so\.6|libgcc_s\.so\.1|libc\.so\.6|ld-linux[-a-z0-9_.]*\.so\.[0-9])$' &&
        fail "$1 needs a library outside the host, libc, libstdc++, OpenSSL 3 and zlib"
    ! readelf -d "$1" | grep -qE '\((RPATH|RUNPATH)\)' || fail "$1 has an RPATH: it must find nothing outside the host"
    ! readelf -S "$1" | grep -q '\.debug_info' || fail "$1 still carries its debug information"
    exports=$(nm -D --defined-only "$1" | awk '{print $3}' | sort | tr '\n' ' ')
    glibc=$(objdump -T "$1" | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1)
    echo "  exports $exports; newest glibc symbol $glibc"
    case "$2" in
        binja) [ "$exports" = "CorePluginABIVersion CorePluginInit UIPluginABIVersion UIPluginInit " ] ;;
        ida) [ "$exports" = "PLUGIN " ] ;;
    esac || fail "$1 exports more (or less) than the host's entry points"
}

# check_macos <module> <host>: the same for a Mach-O plugin; OpenSSL must be linked in (no Homebrew at run time)
check_macos() {
    local libs exports
    libs=$(otool -L "$1" | tail -n +2 | awk '{print $1}')
    echo "$libs" | sed 's/^/  links /'
    echo "$libs" | grep -vqE '^(/usr/lib/lib(c\+\+\.1|System\.B|z\.1|objc\.A)\.dylib|/System/Library/Frameworks/.*|@rpath/libbinaryninja(core|ui)\.1\.dylib|@rpath/Qt(Core|Gui|Widgets)\.framework/Versions/A/Qt(Core|Gui|Widgets)|@rpath/libida\.dylib)$' &&
        fail "$1 links a library outside macOS and the host (Homebrew's OpenSSL must be linked in)"
    exports=$(nm -gU "$1" | awk '{print $3}' | sort | tr '\n' ' ')
    echo "  exports $exports"
    case "$2" in
        binja) [ "$exports" = "_CorePluginABIVersion _CorePluginInit _UIPluginABIVersion _UIPluginInit " ] ;;
        ida) [ "$exports" = "_PLUGIN " ] ;;
    esac || fail "$1 exports more (or less) than the host's entry points"
}

smoke() {
    local pkg tmp host n=0
    for pkg in dist/rikugan-"$VERSION"-*.tar.gz; do
        [ -e "$pkg" ] || fail "no package in dist/ (ci.sh plugins makes them)"
        host=$(basename "$pkg" | sed "s/^rikugan-$VERSION-\([a-z]*\)-.*/\1/")
        want=$(awk '{print $1}' "$pkg.sha256")
        got=$( (sha256sum "$pkg" 2>/dev/null || shasum -a 256 "$pkg") | awk '{print $1}')
        [ "$want" = "$got" ] || fail "$pkg does not match its .sha256"
        tmp=$(mktemp -d)
        tar -xzf "$pkg" -C "$tmp"
        echo "ci: $(basename "$pkg"): $(cd "$tmp" && find . -type f | sed 's|^\./||' | tr '\n' ' ')"
        if [ "$OS" = Darwin ]; then
            check_macos "$(ls "$tmp"/*.dylib)" "$host"
        else
            check_linux "$(ls "$tmp"/*.so)" "$host"
        fi
        rm -rf "$tmp"
        n=$((n + 1))
    done
    [ "$n" = 2 ] || fail "expected the two plugins in dist/, found $n"
    echo "ci: smoke ok"
}

tag_check() {
    local tag=${1:?tag} ida
    ida=$(sed -n 's/.*"version": "\([0-9.]*\)".*/\1/p' ida-plugin.json | head -n1)
    [ "$tag" = "v$VERSION" ] || fail "tag $tag does not match the project version $VERSION (CMakeLists.txt): tag v$VERSION"
    [ "$ida" = "$VERSION" ] || fail "ida-plugin.json says $ida, CMakeLists.txt $VERSION"
    echo "ci: tag $tag matches"
}

dist_assets() {
    ls dist/rikugan-"$VERSION"-*.tar.gz dist/rikugan-"$VERSION"-*.tar.gz.sha256 \
        dist/rikugan-"$VERSION"-*.zip dist/rikugan-"$VERSION"-*.zip.sha256 2>/dev/null || true
}

case "${1:-}" in
deps) deps ;;
omc) omc ;;
libomc) libomc ;;
test) test_suite ;;
plugins) plugins ;;
smoke) smoke ;;
tag-check) tag_check "${2:-}" ;;
dist-assets) dist_assets ;;
*) fail "usage: $0 deps|omc|libomc|test|plugins|smoke|tag-check <tag>|dist-assets" ;;
esac
