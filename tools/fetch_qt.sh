#!/bin/sh
# fetch_qt.sh <version> [dest]  e.g.  tools/fetch_qt.sh 6.11.1   (Binary Ninja)   tools/fetch_qt.sh 6.8.2   (IDA 9.3)
# Official Qt desktop build (qtbase + its ICU) for this machine from download.qt.io, checked against the published
# SHA-1, unpacked into <dest>/<version>/<kit> (default ~/Qt). Only headers, moc/rcc/uic and CMake files are used at
# build time: the plugin binds to the host's own Qt at runtime. Needs curl, and bsdtar (macOS's tar; Linux: the
# libarchive-tools package) or 7-Zip (7zz / 7z) to read Qt's .7z archives.
set -eu
VER=${1:?usage: fetch_qt.sh <version> [dest]}
DEST=${2:-$HOME/Qt}
command -v curl >/dev/null || { echo "fetch_qt.sh: curl is missing" >&2; exit 1; }
if command -v bsdtar >/dev/null; then UNPACK=bsdtar
elif tar --version 2>/dev/null | grep -q bsdtar; then UNPACK=tar
else UNPACK=$(command -v 7zz || command -v 7z || command -v 7za || true)
fi
[ -n "$UNPACK" ] || { echo "fetch_qt.sh: nothing here reads .7z archives (Linux: install libarchive-tools or 7zip)" >&2; exit 1; }
case "$(uname -s)-$(uname -m)" in
    Linux-aarch64) HOST=linux_arm64; KIT=gcc_arm64; PKG=linux_gcc_arm64 ;;
    Linux-x86_64)  HOST=linux_x64;   KIT=gcc_64;    PKG=linux_gcc_64 ;;
    Darwin-*)      HOST=mac_x64;     KIT=macos;     PKG=clang_64 ;;
    *) echo "fetch_qt.sh: unsupported platform $(uname -s)-$(uname -m)" >&2; exit 1 ;;
esac
NODOT=$(echo "$VER" | tr -d .)
BASE="https://download.qt.io/online/qtsdkrepository/$HOST/desktop/qt6_$NODOT/qt6_$NODOT/qt.qt6.$NODOT.$PKG"
OUT="$DEST/$VER/$KIT"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

LISTING=$(curl -sfL "$BASE/") || { echo "fetch_qt.sh: cannot reach $BASE" >&2; exit 1; }
archives=$(printf '%s' "$LISTING" | grep -oE 'href="[^"]*(qtbase|icu)[^"]*\.7z"' | sed 's/href="//; s/"$//' | sort -u)
[ -n "$archives" ] || { echo "fetch_qt.sh: no qtbase archive at $BASE" >&2; exit 1; }

# unpack <archive> <dir>; tar/bsdtar keep Qt's symlinks, 7-Zip skips some of them (fixed below). Errors are shown,
# the check at the end decides.
unpack() {
    case "$UNPACK" in
        *tar) (cd "$2" && "$UNPACK" -xf "$1") || echo "fetch_qt.sh: $UNPACK reported errors on $(basename "$1")" >&2 ;;
        # -snld (keep links that leave the folder) is unknown to old p7zip builds: retry without it
        *)    (cd "$2" && "$UNPACK" x -y -snld "$1" >"$TMP/7z.log" 2>&1) || (cd "$2" && "$UNPACK" x -y "$1" >"$TMP/7z.log" 2>&1) ||
              { echo "fetch_qt.sh: $UNPACK reported errors on $(basename "$1"):" >&2; tail -n 5 "$TMP/7z.log" >&2; } ;;
    esac
}

mkdir -p "$OUT/lib"
for a in $archives; do
    curl -sfL -o "$TMP/$a" "$BASE/$a" || { echo "fetch_qt.sh: download failed: $BASE/$a" >&2; exit 1; }
    want=$(curl -sfL "$BASE/$a.sha1" | awk '{print $1}')
    got=$( (sha1sum "$TMP/$a" 2>/dev/null || shasum "$TMP/$a") | awk '{print $1}')
    [ "$want" = "$got" ] || { echo "fetch_qt.sh: SHA-1 mismatch for $a" >&2; exit 1; }
    case "$a" in
        *icu*) unpack "$TMP/$a" "$OUT/lib" ;;
        *)     unpack "$TMP/$a" "$OUT" ;;
    esac
done
# 7-Zip skips .so links that point at other links: recreate any that are missing (Linux; macOS has frameworks)
for so6 in "$OUT"/lib/libQt6*.so.6; do
    [ -e "$so6" ] || continue
    so=${so6%.6}
    { [ -L "$so" ] || [ -s "$so" ]; } || ln -sf "$(basename "$so6")" "$so"
done
[ -f "$OUT/lib/cmake/Qt6/Qt6Config.cmake" ] && [ -x "$OUT/libexec/moc" ] ||
    { echo "fetch_qt.sh: the Qt archive did not unpack into $OUT with $UNPACK (no lib/cmake/Qt6 or libexec/moc)" >&2; exit 1; }
"$OUT/libexec/moc" -v >/dev/null
echo "$OUT"
