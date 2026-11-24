#!/bin/sh
# build_omc.sh <OMC_DIR> <OUT_O> <REPO_DIR>
# Builds omc's objects in omc's own per-platform tree and prelinks them into one relocatable object whose only
# global symbols are the public omc_* API (what omc's `make lib` does; see ../Rios/Makefile). Writes the
# libraries the object needs to <OUT_O>.libs.
set -eu
OMC_DIR=$1
OUT_O=$2
REPO=$3
OS=$(uname -s)
ARCH=$(uname -m)
OUT_DIR=$(dirname "$OUT_O")
mkdir -p "$OUT_DIR"

MAKE_ENV=""
if [ "$OS" = Darwin ]; then
    # the plugin must not depend on keg-only Homebrew OpenSSL at runtime
    MAKE_ENV="STATIC_OPENSSL=1"
fi

# shellcheck disable=SC2086
env $MAKE_ENV make -s -C "$OMC_DIR" "build/obj/$OS-$ARCH/omc"
VARS="make -s -C $OMC_DIR -f Makefile -f $REPO/mk/omc-vars.mk"
# shellcheck disable=SC2086
OBJS=$(env $MAKE_ENV $VARS rios-lib-obj)
# shellcheck disable=SC2086
LIBS=$(env $MAKE_ENV $VARS rios-libs)

SYMS="$OUT_DIR/omc.syms"
grep -o 'OMC_API [^(]*(' "$OMC_DIR/include/omc.h" | sed -E 's/.*[ *]([a-z_0-9]+)\($/\1/' | grep '^omc_' | sort -u > "$SYMS"

ABS_OBJS=""
for o in $OBJS; do ABS_OBJS="$ABS_OBJS $OMC_DIR/$o"; done

# nothing to do when the prelinked object is newer than every omc object and the header
if [ -f "$OUT_O" ] && [ -z "$(find $ABS_OBJS "$OMC_DIR/include/omc.h" -newer "$OUT_O" 2>/dev/null | head -n 1)" ]; then
    printf '%s\n' "$LIBS" > "$OUT_O.libs"
    exit 0
fi

if [ "$OS" = Darwin ]; then
    sed 's/^/_/' "$SYMS" > "$SYMS.darwin"
    # shellcheck disable=SC2086
    ld -r -exported_symbols_list "$SYMS.darwin" -o "$OUT_O" $ABS_OBJS
else
    # shellcheck disable=SC2086
    ld -r -o "$OUT_DIR/omc-prelink.o" $ABS_OBJS
    objcopy --keep-global-symbols="$SYMS" "$OUT_DIR/omc-prelink.o" "$OUT_O"
    rm -f "$OUT_DIR/omc-prelink.o"
fi
printf '%s\n' "$LIBS" > "$OUT_O.libs"
