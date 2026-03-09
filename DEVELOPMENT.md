# Development Guide

> If you are a coding agent, please read [AGENTS.md](AGENTS.md) instead.

This document is for human contributors. It covers how to set up a development environment, the branch workflow, and what to do before opening a PR.

---

## Prerequisites

- **Binary Ninja** (build 3164 or newer) and/or **IDA Pro 9.x**
- **Python 3.10–3.11** recommended (see note below on IDA Pro + Python versions)
- **Git**
- An API key for at least one supported LLM provider (Anthropic, OpenAI, Google, or a local Ollama instance)

> **IDA Pro note:** Python 3.10 is the safest choice. Higher versions may trigger a Shiboken UAF crash during Qt signal dispatch. See the IDA API Notes section of AGENTS.md for details.

---

## Installation (Development)

Clone the repo and symlink it into the host's plugin directory so changes take effect on the next launch without reinstalling.

**Binary Ninja**
```bash
# macOS
git clone https://github.com/buzzer-re/rikugan
ln -s "$(pwd)/rikugan" ~/Library/Application\ Support/Binary\ Ninja/plugins/rikugan

# Linux
git clone https://github.com/buzzer-re/rikugan
ln -s "$(pwd)/rikugan" ~/.binaryninja/plugins/rikugan

# Windows (run as Administrator)
git clone https://github.com/buzzer-re/rikugan
mklink /D "%APPDATA%\Binary Ninja\plugins\rikugan" "<full path to cloned repo>"
```

**IDA Pro**
```bash
# macOS / Linux
ln -s "$(pwd)/rikugan" ~/.idapro/plugins/rikugan

# Windows
mklink /D "%APPDATA%\Hex-Rays\IDA Pro\plugins\rikugan" "<full path to cloned repo>"
```

---

## Python Dependencies

Install the runtime dependencies into the Python environment used by your host:

```bash
pip install anthropic>=0.39.0 openai>=1.50.0 google-genai>=1.0.0 tomli>=2.0.0
```

For development tooling (CI checks, running tests locally):

```bash
pip install ruff mypy pytest desloppify
```

---

## Branch Workflow

```
feat/my-thing  ─┐
fix/some-bug   ─┤──► dev ──► main
chore/deps     ─┘
```

1. Branch off `dev` using a descriptive prefix:
   - `feat/` — new feature
   - `fix/` — bug fix
   - `refactor/` — code restructure, no behavior change
   - `chore/` — deps, tooling, docs
2. Make your changes in small, focused commits
3. Run the local CI script (see below) before pushing
4. Open a PR targeting `dev`
5. Once reviewed and CI passes, it gets merged to `dev`
6. Releases go from `dev` → `main` with a version tag

**Direct pushes to `main` are not allowed** — must go through a PR. `dev` accepts direct pushes.

---

## Before Pushing — Local CI Check

Run this script after every feature or fix, before opening a PR:

```bash
./ci-local.sh
```

This mirrors exactly what GitHub Actions runs. It will catch formatting errors, lint issues, type errors, test failures, and code quality regressions before they reach CI.

If ruff reports formatting issues, auto-fix them:

```bash
./ci-local.sh --fix
```

The script installs `ruff` and `mypy` if they are not already available. It skips steps whose tools are missing rather than failing hard, so it is safe to run in a partial environment.

---

## Running Tests

```bash
python3 -m pytest tests/ -v
```

Tests are organized under `tests/` by subsystem:

```
tests/
├── agent/       # Agent loop, plan mode, exploration, session
├── core/        # Config, sanitize, errors, profile, logging
├── providers/   # All LLM providers
├── tools/       # Tool implementations (binja, IDA, shared)
└── mocks/       # ida_mock — stubs the IDA Pro API for testing outside IDA
```

Binary Ninja and IDA Pro APIs are stubbed at test time — you do not need either host installed to run the test suite.

---

## Code Quality

This project uses [desloppify](https://github.com/peteromallet/desloppify) to track codebase health. The current objective score is **90.3/100** (target: 95).

Run a scan locally at any time:

```bash
desloppify scan
desloppify status   # score dashboard
desloppify issues   # work queue of findings
```

The `desloppify review` command (subjective scoring) uses an LLM and is run manually before releases, not on every change.

---

## Commit Style

```
feat(agent): add streaming cancellation for plan mode
fix(binja): handle missing function at cursor gracefully
refactor(providers): extract retry logic into base class
security: strip homoglyph sequences in sanitize.py
docs: update tool registration guide in AGENTS.md
```

Format: `type(scope): short description`
- One logical change per commit
- Scope is the subsystem: `agent`, `binja`, `ida`, `ui`, `providers`, `mcp`, `skills`, `core`

---

## Release Process

1. Merge `dev` → `main` via PR
2. Bump `version` in `plugin.json`
3. Tag and push:
   ```bash
   git tag v0.x.x
   git push origin v0.x.x
   ```
4. GitHub Actions validates the tag matches `plugin.json` and publishes the GitHub Release
5. Binary Ninja plugin manager picks up the new version from `main` automatically

---

## Getting Help

- Read [AGENTS.md](AGENTS.md) for deep technical documentation on internals, architecture decisions, and coding rules
- Open an issue at https://github.com/buzzer-re/rikugan/issues
