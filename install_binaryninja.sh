#!/usr/bin/env bash
# Rikugan installer for Binary Ninja (Linux/macOS)
# Usage: ./install_binaryninja.sh [BN_USER_DIR]
#   BN_USER_DIR  Optional path to Binary Ninja user directory

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

find_bn_user_dir() {
    local candidates=()
    if [[ "$(uname)" == "Darwin" ]]; then
        candidates+=("$HOME/Library/Application Support/Binary Ninja" "$HOME/.binaryninja")
    else
        candidates+=("$HOME/.binaryninja")
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
    BN_USER_DIR="$1"
    if [[ ! -d "$BN_USER_DIR" ]]; then
        err "Provided Binary Ninja directory does not exist: $BN_USER_DIR"
        exit 1
    fi
    info "Using provided Binary Ninja directory: $BN_USER_DIR"
else
    if BN_USER_DIR="$(find_bn_user_dir)"; then
        info "Auto-detected Binary Ninja directory: $BN_USER_DIR"
    else
        BN_USER_DIR="$HOME/.binaryninja"
        warn "No Binary Ninja directory found, defaulting to $BN_USER_DIR"
    fi
fi

PLUGINS_DIR="$BN_USER_DIR/plugins"
CONFIG_DIR="$BN_USER_DIR/rikugan"
SKILLS_DIR="$CONFIG_DIR/skills"
PLUGIN_LINK="$PLUGINS_DIR/rikugan"

# ── Remove old "iris" installation (rebrand cleanup) ─────────────────
OLD_LINK="$PLUGINS_DIR/iris"
if [[ -L "$OLD_LINK" ]]; then
    warn "Removing old 'iris' plugin symlink: $OLD_LINK"
    rm "$OLD_LINK"
    ok "Old 'iris' symlink removed"
elif [[ -d "$OLD_LINK" ]]; then
    warn "Removing old 'iris' plugin directory: $OLD_LINK"
    rm -rf "$OLD_LINK"
    ok "Old 'iris' directory removed"
fi

if [[ ! -f "$SCRIPT_DIR/rikugan_binaryninja.py" ]] || [[ ! -f "$SCRIPT_DIR/plugin.json" ]]; then
    err "Binary Ninja plugin files missing in $SCRIPT_DIR"
    exit 1
fi

find_bn_install_dir() {
    # Find Binary Ninja's installation directory
    local candidates=()
    if [[ "$(uname)" == "Darwin" ]]; then
        candidates+=(
            "/Applications/Binary Ninja.app"
            "$HOME/Applications/Binary Ninja.app"
        )
    else
        # Linux common install locations
        candidates+=(
            "$HOME/binaryninja"
            "/opt/binaryninja"
            "/usr/local/binaryninja"
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

try_bn_python_pip() {
    # Attempt to run pip using Binary Ninja's bundled Python
    local bn_install="$1"
    local req="$2"

    if [[ "$(uname)" == "Darwin" ]]; then
        # macOS: Python binary is in the Framework, stdlib in bundled-python3
        local bn_python="$bn_install/Contents/Frameworks/Python.framework/Versions/Current/bin/python3"
        local bn_home="$bn_install/Contents/Resources/bundled-python3"
        if [[ -x "$bn_python" ]] && [[ -d "$bn_home" ]]; then
            info "Found Binary Ninja Python: $bn_python"
            if PYTHONHOME="$bn_home" "$bn_python" -m pip install -r "$req" 2>&1; then
                return 0
            fi
            warn "pip install failed with BN Python"
        fi
    else
        # Linux: BN uses system Python, no bundled interpreter.
        # Packages are loaded from ~/.binaryninja/python3XX/ at runtime.
        info "Linux: Binary Ninja uses system Python"
    fi
    return 1
}

install_requirements() {
    local req="$SCRIPT_DIR/requirements.txt"

    # 1. Explicit override via BN_PYTHON env var
    if [[ -n "${BN_PYTHON:-}" ]]; then
        if "$BN_PYTHON" -m pip install -r "$req"; then
            ok "Dependencies installed with BN_PYTHON override"
            return 0
        fi
        warn "BN_PYTHON override failed, trying other methods..."
    fi

    # 2. Try Binary Ninja's bundled Python (macOS only; Linux has no bundled Python)
    local bn_install
    if bn_install="$(find_bn_install_dir)"; then
        info "Found Binary Ninja at: $bn_install"
        if try_bn_python_pip "$bn_install" "$req"; then
            ok "Dependencies installed into Binary Ninja's Python"
            return 0
        fi
    fi

    # 3. Fallback: system Python (needed for Linux, last resort for macOS)
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
    err "Failed to install Python dependencies from requirements.txt"
    exit 1
fi

mkdir -p "$PLUGINS_DIR"
mkdir -p "$SKILLS_DIR"

# Built-in skills are loaded directly from rikugan/skills/builtins/ (via symlink).
# The user skills directory is for user-created skills only.
# Remove stale built-in copies that previous installs may have placed here.
BUILTINS_SRC="$SCRIPT_DIR/rikugan/skills/builtins"
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

if [[ -L "$PLUGIN_LINK" ]]; then
    existing="$(readlink "$PLUGIN_LINK")"
    if [[ "$existing" == "$SCRIPT_DIR" ]]; then
        ok "Plugin symlink already installed"
    else
        warn "Removing stale symlink: $PLUGIN_LINK -> $existing"
        rm "$PLUGIN_LINK"
        ln -s "$SCRIPT_DIR" "$PLUGIN_LINK"
        ok "Plugin symlink updated: $PLUGIN_LINK -> $SCRIPT_DIR"
    fi
elif [[ -e "$PLUGIN_LINK" ]]; then
    warn "Backing up existing plugin directory to ${PLUGIN_LINK}.bak"
    mv "$PLUGIN_LINK" "${PLUGIN_LINK}.bak"
    ln -s "$SCRIPT_DIR" "$PLUGIN_LINK"
    ok "Plugin symlink created: $PLUGIN_LINK -> $SCRIPT_DIR"
else
    ln -s "$SCRIPT_DIR" "$PLUGIN_LINK"
    ok "Plugin symlink created: $PLUGIN_LINK -> $SCRIPT_DIR"
fi

echo ""
ok "Rikugan Binary Ninja plugin installed successfully!"
info "Plugin: $PLUGIN_LINK"
info "Config: $CONFIG_DIR/"
info "Skills: $SKILLS_DIR/"
echo ""
info "Restart Binary Ninja. Open: Tools -> Rikugan -> Open Panel."
