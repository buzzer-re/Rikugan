#!/usr/bin/env bash
# Local CI simulation — mirrors the GitHub Actions pipeline
# Usage: ./ci-local.sh [--fix]
set -euo pipefail

FIX=false
[[ "${1:-}" == "--fix" ]] && FIX=true

ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

PASS=0
FAIL=0
RESULTS=()

# ── Colours ────────────────────────────────────────────────────────────────────
GREEN='\033[0;32m'; RED='\033[0;31m'; YELLOW='\033[1;33m'; RESET='\033[0m'
BOLD='\033[1m'

ok()   { PASS=$((PASS+1)); RESULTS+=("${GREEN}✔${RESET} $1"); }
fail() { FAIL=$((FAIL+1)); RESULTS+=("${RED}✘${RESET} $1: $2"); }
info() { echo -e "${YELLOW}▶ $1${RESET}"; }

# ── Tool bootstrap ─────────────────────────────────────────────────────────────
info "Checking tools..."
NEED=()
python3 -m ruff --version &>/dev/null || NEED+=(ruff)
python3 -m mypy --version &>/dev/null || NEED+=(mypy)

if [[ ${#NEED[@]} -gt 0 ]]; then
    echo "  Installing missing tools: ${NEED[*]}"
    pip3 install --quiet "${NEED[@]}"
fi

# ── 1. Ruff — format check ─────────────────────────────────────────────────────
info "[1/5] Ruff format..."
if $FIX; then
    python3 -m ruff format rikugan/ && ok "ruff format (auto-fixed)" || fail "ruff format" "failed"
else
    if python3 -m ruff format --check rikugan/ 2>&1; then
        ok "ruff format"
    else
        fail "ruff format" "run with --fix to auto-fix"
    fi
fi

# ── 2. Ruff — lint ────────────────────────────────────────────────────────────
info "[2/5] Ruff lint..."
RUFF_ARGS=()
if $FIX; then
    RUFF_ARGS+=(--fix)
fi

if python3 -m ruff check rikugan/ "${RUFF_ARGS[@]}" 2>&1; then
    ok "ruff lint"
else
    fail "ruff lint" "see above"
fi

# ── 3. Mypy — core modules only ───────────────────────────────────────────────
info "[3/5] Mypy (core + providers)..."
MYPY_OUT=$(python3 -m mypy rikugan/core rikugan/providers --pretty \
    2>&1) && MYPY_OK=true || MYPY_OK=false

if $MYPY_OK; then
    ok "mypy"
else
    # Only count as failure if there are actual errors (not just notes)
    ERROR_COUNT=$(echo "$MYPY_OUT" | grep -c "^.*: error:" || true)
    if [[ $ERROR_COUNT -gt 0 ]]; then
        echo "$MYPY_OUT"
        fail "mypy" "$ERROR_COUNT error(s)"
    else
        ok "mypy (warnings only)"
    fi
fi

# ── 4. Pytest ─────────────────────────────────────────────────────────────────
info "[4/5] Pytest..."
if python3 -m pytest --version &>/dev/null; then
    if python3 -m pytest tests/ --tb=short -q 2>&1; then
        ok "pytest"
    else
        fail "pytest" "see above"
    fi
else
    RESULTS+=("${YELLOW}⚠${RESET} pytest: not installed, skipped")
fi

# ── 5. Desloppify — objective score gate ──────────────────────────────────────
info "[5/5] Desloppify (objective score)..."
if command -v desloppify &>/dev/null; then
    desloppify scan --profile objective --no-badge 2>&1 | tail -5

    SCORE=$(python3 -c "
import json, sys
try:
    data = json.load(open('.desloppify/query.json'))
    print(data.get('objective_score', 0))
except Exception as e:
    print(0)
")
    BASELINE=90.3

    DROPPED=$(python3 -c "print('yes' if float('$SCORE') < $BASELINE - 0.5 else 'no')")
    if [[ "$DROPPED" == "yes" ]]; then
        fail "desloppify" "objective score $SCORE < baseline $BASELINE"
    else
        ok "desloppify (objective: $SCORE/100, baseline: $BASELINE)"
    fi
else
    RESULTS+=("${YELLOW}⚠${RESET} desloppify: not found, skipped")
fi

# ── Summary ───────────────────────────────────────────────────────────────────
echo ""
echo -e "${BOLD}── CI Results ──────────────────────────────────────────${RESET}"
for r in "${RESULTS[@]}"; do
    echo -e "  $r"
done
echo ""

if [[ $FAIL -gt 0 ]]; then
    echo -e "${RED}${BOLD}FAILED${RESET} — $FAIL check(s) failed, $PASS passed"
    exit 1
else
    echo -e "${GREEN}${BOLD}ALL PASSED${RESET} — $PASS checks"
fi
