#!/usr/bin/env bash
# Rikugan installer for Linux and macOS
# Usage: ./install.sh [IDA_USER_DIR]
#   IDA_USER_DIR  Optional path to IDA user directory (default: auto-detect)

set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
CYAN='\033[0;36m'
NC='\033[0m'

info()  { printf "${CYAN}[*]${NC} %s\n" "$*"; }
ok()    { printf "${GREEN}[+]${NC} %s\n" "$*"; }
warn()  { printf "${YELLOW}[!]${NC} %s\n" "$*"; }
err()   { printf "${RED}[-]${NC} %s\n" "$*" >&2; }

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# ── Locate IDA user directory ─────────────────────────────────────────

find_ida_user_dir() {
    # Common locations, in order of preference
    local candidates=()

    if [[ "$(uname)" == "Darwin" ]]; then
        candidates+=(
            "$HOME/.idapro"
            "$HOME/Library/Application Support/Hex-Rays/IDA Pro"
        )
    else
        candidates+=(
            "$HOME/.idapro"
            "$HOME/.ida"
        )
    fi

    for dir in "${candidates[@]}"; do
        if [[ -d "$dir" ]]; then
            echo "$dir"
            return 0
        fi
    done

    return 1
}

if [[ $# -ge 1 ]]; then
    IDA_USER_DIR="$1"
    if [[ ! -d "$IDA_USER_DIR" ]]; then
        err "Provided IDA directory does not exist: $IDA_USER_DIR"
        exit 1
    fi
    info "Using provided IDA directory: $IDA_USER_DIR"
else
    if IDA_USER_DIR="$(find_ida_user_dir)"; then
        info "Auto-detected IDA directory: $IDA_USER_DIR"
    else
        # Fall back to the standard default and create it
        IDA_USER_DIR="$HOME/.idapro"
        warn "No IDA directory found, defaulting to $IDA_USER_DIR"
    fi
fi

PLUGINS_DIR="$IDA_USER_DIR/plugins"
CONFIG_DIR="$IDA_USER_DIR/rikugan"

# ── Remove old "iris" installation (rebrand cleanup) ─────────────────
for old_name in "iris_plugin.py" "iris"; do
    OLD_PATH="$PLUGINS_DIR/$old_name"
    if [[ -L "$OLD_PATH" ]]; then
        warn "Removing old '$old_name' symlink: $OLD_PATH"
        rm "$OLD_PATH"
        ok "Old '$old_name' symlink removed"
    elif [[ -e "$OLD_PATH" ]]; then
        warn "Removing old '$old_name': $OLD_PATH"
        rm -rf "$OLD_PATH"
        ok "Old '$old_name' removed"
    fi
done

# ── Sanity checks ─────────────────────────────────────────────────────

if [[ ! -f "$SCRIPT_DIR/rikugan_plugin.py" ]]; then
    err "rikugan_plugin.py not found in $SCRIPT_DIR — run this from the repo root"
    exit 1
fi

if [[ ! -d "$SCRIPT_DIR/rikugan" ]]; then
    err "rikugan/ package not found in $SCRIPT_DIR — run this from the repo root"
    exit 1
fi

# ── Find IDA installation directory ───────────────────────────────────

_normalize_ida_install_dir() {
    local path="$1"
    [[ -n "$path" ]] || return 1

    if [[ "$(uname)" == "Darwin" ]]; then
        if [[ -d "$path/Contents/MacOS" ]]; then
            echo "$path/Contents/MacOS"
            return 0
        fi
    fi

    [[ -d "$path" ]] || return 1
    echo "$path"
}

find_ida_install_dir() {
    # Check IDADIR env var first
    if [[ -n "${IDADIR:-}" ]]; then
        local normalized
        if normalized="$(_normalize_ida_install_dir "$IDADIR")"; then
            echo "$normalized"
            return 0
        fi
    fi

    # PATH is a stronger signal than arbitrary filesystem scanning.
    local ida_bin
    for name in ida64 idat64 ida idat; do
        if ida_bin="$(command -v "$name" 2>/dev/null)"; then
            local real_bin
            real_bin="$(readlink -f "$ida_bin" 2>/dev/null || realpath "$ida_bin" 2>/dev/null || echo "$ida_bin")"
            local normalized
            if normalized="$(_normalize_ida_install_dir "$(dirname "$real_bin")")"; then
                echo "$normalized"
                return 0
            fi
        fi
    done

    local candidates=()
    if [[ "$(uname)" == "Darwin" ]]; then
        # macOS: IDA .app bundles — the actual binaries are in Contents/MacOS
        for app in /Applications/IDA*.app; do
            [[ -d "$app" ]] && candidates+=("$app")
        done
        for app in "$HOME/Applications/IDA"*.app; do
            [[ -d "$app" ]] && candidates+=("$app")
        done
    else
        # Linux common install locations
        candidates+=(
            /opt/ida*
            /opt/idapro*
            "$HOME/ida"*
            "$HOME/idapro"*
        )
    fi

    local dir
    for dir in "${candidates[@]}"; do
        local normalized
        if normalized="$(_normalize_ida_install_dir "$dir" 2>/dev/null)"; then
            echo "$normalized"
            return 0
        fi
    done

    return 1
}

# ── Find IDA's Python ────────────────────────────────────────────────

_find_host_python() {
    local py
    for py in python3 python; do
        if command -v "$py" >/dev/null 2>&1; then
            command -v "$py"
            return 0
        fi
    done
    return 1
}

_extract_python_version() {
    local target="$1"
    local version

    version="$(printf '%s\n' "$target" | sed -nE 's@.*/Versions/([0-9]+\.[0-9]+)/.*@\1@p' | head -n 1)"
    if [[ -n "$version" ]]; then
        echo "$version"
        return 0
    fi

    version="$(basename "$target" | sed -nE 's/^libpython([0-9]+\.[0-9]+).*/\1/p' | head -n 1)"
    if [[ -n "$version" ]]; then
        echo "$version"
        return 0
    fi

    return 1
}

# A framework dylib such as
#   .../Python.framework/Versions/3.14/Python
# carries the executable bit, so testing -x alone accepted a shared library as
# the interpreter and the install died with "cannot execute binary file".
# Ask the candidate to behave like Python instead of trusting its mode bits.
_is_python_interpreter() {
    local candidate="${1:-}"
    [[ -n "$candidate" && -f "$candidate" && -x "$candidate" ]] || return 1
    # Require real output, not just a zero exit: a wrapper script that ignores
    # its arguments would otherwise pass and take the dependencies with it.
    local marker
    marker="$("$candidate" -c 'import sys; print("RIKUGAN_PY%d" % sys.version_info[0])' 2>/dev/null)" || return 1
    [[ "$marker" == "RIKUGAN_PY3" ]]
}

_python_target_to_interpreter() {
    # Given the configured Python shared library path, find the matching interpreter.
    local target="$1"
    [[ -n "$target" ]] || return 1

    if _is_python_interpreter "$target"; then
        echo "$target"
        return 0
    fi

    local target_dir target_name version
    target_dir="$(dirname "$target")"
    target_name="$(basename "$target")"
    version="$(_extract_python_version "$target" || true)"
    local candidates=()

    # Framework layout (macOS): <framework>/Versions/X.Y/bin/python3
    if [[ "$target_name" == "Python" ]]; then
        [[ -n "$version" ]] && candidates+=("$target_dir/bin/python$version")
        candidates+=("$target_dir/bin/python3" "$target_dir/bin/python")
    fi

    # Shared lib layout (Linux): interpreter may live in ../bin/ or on PATH as pythonX.Y.
    if [[ "$target_name" == libpython* ]]; then
        [[ -n "$version" ]] && candidates+=(
            "$target_dir/../bin/python$version"
            "$target_dir/python$version"
            "/usr/bin/python$version"
            "/usr/local/bin/python$version"
            "/bin/python$version"
        )
        candidates+=(
            "$target_dir/../bin/python3"
            "$target_dir/python3"
            "$target_dir/../bin/python"
            "$target_dir/python"
        )
    fi

    if [[ -n "$version" ]]; then
        local path_python
        path_python="$(command -v "python$version" 2>/dev/null || true)"
        [[ -n "$path_python" ]] && candidates+=("$path_python")
    fi

    local pybin
    for pybin in "${candidates[@]}"; do
        if _is_python_interpreter "$pybin"; then
            echo "$pybin"
            return 0
        fi
    done

    return 1
}

_read_ida_reg_python() {
    # Read Python3TargetDLL from IDA's binary registry file (~/.idapro/ida.reg).
    # Format: null-terminated key, 4-byte LE length, 1-byte type, then 'length' bytes of value.
    local reg_file="$1"
    [[ -f "$reg_file" ]] || return 1
    local parser
    parser="$(_find_host_python)" || return 1

    "$parser" -c "
import sys
with open(sys.argv[1], 'rb') as f:
    data = f.read()
idx = data.find(b'Python3TargetDLL')
if idx < 0:
    sys.exit(1)
key_end = data.index(b'\x00', idx)
length = int.from_bytes(data[key_end+1:key_end+5], 'little')
if length <= 0 or length > 1024:
    sys.exit(1)
value = data[key_end+6:key_end+6+length]
# Decode and strip any trailing nulls
path = value.decode('utf-8', errors='replace').rstrip('\x00')
if path.startswith('/') or path.startswith('\\\\'):
    print(path)
" "$reg_file" 2>/dev/null
}

_find_bundled_ida_python() {
    local ida_install="$1"

    # Bundled Python: <IDA>/python3*/python3 (IDA 7.5+, some Linux builds)
    for pydir in "$ida_install"/python3*/; do
        if [[ -x "$pydir/python3" ]]; then
            echo "$pydir/python3"
            return 0
        elif [[ -x "$pydir/python" ]]; then
            echo "$pydir/python"
            return 0
        fi
    done

    # Older bundled layout: <IDA>/python/python3
    if [[ -x "$ida_install/python/python3" ]]; then
        echo "$ida_install/python/python3"
        return 0
    elif [[ -x "$ida_install/python/python" ]]; then
        echo "$ida_install/python/python"
        return 0
    fi

    # macOS bundled framework layout: <IDA>.app/Contents/Frameworks/Python.framework/Versions/*/bin/python3
    if [[ "$(uname)" == "Darwin" ]] && [[ "$ida_install" == */Contents/MacOS ]]; then
        local contents_dir="${ida_install%/MacOS}"
        for pybin in \
            "$contents_dir/Frameworks/Python.framework/Versions/Current/bin/python3" \
            "$contents_dir/Frameworks/Python.framework/Versions/Current/bin/python" \
            "$contents_dir"/Frameworks/Python.framework/Versions/*/bin/python3 \
            "$contents_dir"/Frameworks/Python.framework/Versions/*/bin/python; do
            if [[ -x "$pybin" ]]; then
                echo "$pybin"
                return 0
            fi
        done
    fi

    return 1
}

_find_ida_configured_python() {
    local ida_install="${1:-}"

    # Read Python3TargetDLL from ida.reg first. This reflects the Python that the
    # current IDA user profile is configured to load, which is more accurate than
    # guessing from the filesystem when multiple IDA versions are installed.
    local ida_reg="$IDA_USER_DIR/ida.reg"
    local python_target
    if python_target="$(_read_ida_reg_python "$ida_reg")" && [[ -n "$python_target" ]]; then
        local interp
        if interp="$(_python_target_to_interpreter "$python_target")"; then
            echo "$interp"
            return 0
        fi
    fi

    [[ -n "$ida_install" ]] || return 1

    # Fall back to idapyswitch if ida.reg is missing or does not map cleanly.
    local idapyswitch="$ida_install/idapyswitch"
    if [[ -x "$idapyswitch" ]]; then
        local python_target
        python_target="$("$idapyswitch" --dry-run --auto-apply --verbose 2>&1 \
            | sed -n "s/.*Setting registry value Python3TargetDLL to '\\(.*\\)'/\\1/p" \
            | head -n 1 || true)"
        if [[ -n "$python_target" ]]; then
            local interp
            if interp="$(_python_target_to_interpreter "$python_target")"; then
                echo "$interp"
                return 0
            fi
        fi
    fi

    return 1
}

find_ida_python() {
    local ida_install="${1:-}"

    local ida_python
    if ida_python="$(_find_ida_configured_python "$ida_install")"; then
        echo "$ida_python"
        return 0
    fi

    if [[ -n "$ida_install" ]] && ida_python="$(_find_bundled_ida_python "$ida_install")"; then
        echo "$ida_python"
        return 0
    fi

    return 1
}

# ── Install dependencies ──────────────────────────────────────────────

install_requirements() {
    local req="$SCRIPT_DIR/requirements.txt"

    # 1. Explicit override via IDA_PYTHON env var
    if [[ -n "${IDA_PYTHON:-}" ]]; then
        if "$IDA_PYTHON" -m pip install -r "$req"; then
            ok "Dependencies installed with IDA_PYTHON override"
            return 0
        fi
        warn "IDA_PYTHON override failed, trying other methods..."
    fi

    # 2. Try IDA's bundled/configured Python
    local ida_install=""
    ida_install="$(find_ida_install_dir || true)"
    if [[ -n "$ida_install" ]]; then
        info "Found IDA installation at: $ida_install"
    fi

    local ida_python
    if ida_python="$(find_ida_python "$ida_install")"; then
        info "Using IDA's Python: $ida_python"
        if "$ida_python" -m pip install --break-system-packages -r "$req" 2>/dev/null \
           || "$ida_python" -m pip install -r "$req"; then
            ok "Dependencies installed into IDA's Python"
            return 0
        fi
        warn "pip install failed with IDA's Python, trying system fallbacks..."
    elif [[ -n "$ida_install" ]]; then
        warn "Could not find IDA's configured or bundled Python, trying system fallbacks..."
    else
        warn "Could not find IDA installation directory, trying system Python..."
    fi

    # 3. Fallback: system Python
    local fallbacks=("python3 -m pip" "python -m pip" "pip3" "pip")
    for cmd in "${fallbacks[@]}"; do
        if eval "$cmd --version" >/dev/null 2>&1; then
            info "Installing Python dependencies with: $cmd"
            if eval "$cmd install --break-system-packages -r \"$req\"" 2>/dev/null \
               || eval "$cmd install --user -r \"$req\"" 2>/dev/null \
               || eval "$cmd install -r \"$req\""; then
                ok "Dependencies installed successfully"
                return 0
            fi
            warn "Dependency install failed with: $cmd"
        fi
    done
    return 1
}

if ! install_requirements; then
    warn "Could not install one or more Python dependencies."
    warn "Rikugan will still be installed, but features tied to missing packages will show warnings in the plugin."
fi

# ── Create directories ────────────────────────────────────────────────

mkdir -p "$PLUGINS_DIR"
mkdir -p "$CONFIG_DIR"

# ── Copy built-in skills ──────────────────────────────────────────────

SKILLS_DIR="$CONFIG_DIR/skills"
BUILTINS_SRC="$SCRIPT_DIR/rikugan/skills/builtins"

# Built-in skills are loaded directly from rikugan/skills/builtins/ (via symlink).
# The user skills directory is for user-created skills only.
# Remove stale built-in copies that previous installs may have placed here.
if [[ -d "$BUILTINS_SRC" ]] && [[ -d "$SKILLS_DIR" ]]; then
    for skill in "$BUILTINS_SRC"/*/; do
        slug="$(basename "$skill")"
        dst="$SKILLS_DIR/$slug"
        if [[ -d "$dst" ]]; then
            rm -rf "$dst"
            info "Removed stale built-in copy: /$slug"
        fi
    done
fi

# ── Install plugin via symlinks ───────────────────────────────────────

install_link() {
    local src="$1" dst="$2" name="$3"

    if [[ -L "$dst" ]]; then
        local existing
        existing="$(readlink "$dst")"
        if [[ "$existing" == "$src" ]]; then
            ok "$name already linked"
            return
        fi
        warn "Removing stale symlink: $dst -> $existing"
        rm "$dst"
    elif [[ -e "$dst" ]]; then
        warn "Backing up existing $name to ${dst}.bak"
        mv "$dst" "${dst}.bak"
    fi

    ln -s "$src" "$dst"
    ok "$name -> $dst"
}

info "Installing Rikugan into $PLUGINS_DIR..."
install_link "$SCRIPT_DIR/rikugan_plugin.py" "$PLUGINS_DIR/rikugan_plugin.py" "rikugan_plugin.py"
install_link "$SCRIPT_DIR/rikugan"        "$PLUGINS_DIR/rikugan"        "rikugan/"

# ── Done ──────────────────────────────────────────────────────────────

echo ""
ok "Rikugan installed successfully!"
info "Plugin:  $PLUGINS_DIR/rikugan_plugin.py"
info "Package: $PLUGINS_DIR/rikugan"
info "Config:  $CONFIG_DIR/"
info "Skills:  $SKILLS_DIR/"
echo ""
info "Open IDA and press Ctrl+Shift+I to start Rikugan."
info "First run: click Settings to configure your LLM provider and API key."
info "For Binary Ninja installation, run: ./install_binaryninja.sh"
