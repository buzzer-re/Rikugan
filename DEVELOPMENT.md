# Development Guide

> Coding agents: read [AGENTS.md](AGENTS.md). Internals: [ARCHITECTURE.md](ARCHITECTURE.md).

Rikugan is a native C++20 / Qt Widgets plugin for IDA Pro and Binary Ninja; its agent is libomc, linked in-process.
This guide covers the development setup, builds, tests and the contribution workflow. (Rikugan 1.x was a Python
plugin; it lives only in the git history.)

## Prerequisites

- Linux or macOS.
- CMake ≥ 3.25, Ninja (or make), a C++20 compiler, OpenSSL 3 and zlib headers, `curl` and `7z` (p7zip) for
  `tools/fetch_qt.sh`. Debian/Ubuntu: `build-essential cmake ninja-build libssl-dev zlib1g-dev p7zip-full curl`.
- `python3` for the test suite (omc's mock model server).
- The hosts you build for: IDA Pro 9.3 (default `~/ida-pro-9.3`, override `IDA_DIR`) and/or Binary Ninja (default
  `~/binaryninja`, override `BN_INSTALL_DIR`).

## Checkout

```bash
git clone --recursive https://github.com/buzzer-re/Rikugan
git clone https://github.com/buzzer-re/Oh-My-C ohmyc      # libomc, next to Rikugan (or -DOMC_DIR=...)
cd Rikugan
git submodule update --init --recursive third_party       # if you cloned without --recursive
```

Submodules (`third_party/`):

| Submodule | Pin |
|---|---|
| `binaryninja-api` | the commit in `<BN install>/api_REVISION.txt` |
| `ida-sdk` | `v9.3.0-sdk.3` |

**Binary Ninja ABI pinning.** BN refuses plugins built against a different core/UI ABI. After a BN update, move the
submodule to the new revision and rebuild:

```bash
rev=$(sed 's#.*/##' ~/binaryninja/api_REVISION.txt)
git -C third_party/binaryninja-api fetch origin && git -C third_party/binaryninja-api checkout "$rev"
git add third_party/binaryninja-api
```

## Qt headers

The plugin never ships Qt: it binds to the host's Qt at runtime. At build time it needs headers, `moc`/`rcc` and
CMake files of the matching version, fetched from download.qt.io (SHA-1 checked) into `~/Qt/<version>/<kit>`:

```bash
tools/fetch_qt.sh 6.11.1     # Binary Ninja (also used by the test tree)
tools/fetch_qt.sh 6.8.2      # IDA 9.3 — IDA's Qt is built with -qtnamespace QT; the ida tree compiles with QT_NAMESPACE=QT
```

`<kit>` is `gcc_arm64`, `gcc_64` or `macos`.

## Build trees

One host per tree; `src/common` is compiled into each tree with that tree's Qt.

```bash
cmake -S . -B build/none  -G Ninja -DRIKUGAN_HOST=none  -DCMAKE_PREFIX_PATH=~/Qt/6.11.1/<kit> && cmake --build build/none
cmake -S . -B build/binja -G Ninja -DRIKUGAN_HOST=binja -DCMAKE_PREFIX_PATH=~/Qt/6.11.1/<kit> && cmake --build build/binja
cmake -S . -B build/ida   -G Ninja -DRIKUGAN_HOST=ida   -DCMAKE_PREFIX_PATH=~/Qt/6.8.2/<kit>  && cmake --build build/ida
```

| Tree | Output |
|---|---|
| `build/none` | `rikugan_tests` (no host) |
| `build/binja` | `librikugan.so` / `.dylib` → `~/.binaryninja/plugins/` (macOS: `~/Library/Application Support/Binary Ninja/plugins/`) |
| `build/ida` | `rikugan.so` / `.dylib` → `~/.idapro/plugins/`, plus `rikugan_ida_tooltest` |

Cache variables: `OMC_DIR` (default `../ohmyc`), `BN_INSTALL_DIR`, `IDA_DIR`, `RIKUGAN_BUILD_TESTS` (on for `none`).

- Each build runs `cmake/build_omc.sh`: omc's own `make` builds its objects incrementally, then they are prelinked into
  `build/<tree>/omc/omc-lib.o` with only `omc_*` symbols global. Changes in `../ohmyc` are picked up on the next build.
- `cmake/embed.cmake` compiles every file under `skills/` and `prompts/` into the plugin; editing them needs a rebuild,
  not a reinstall of loose files.
- Files under `src/ida/sdk/` subclass SDK types and are compiled with `-fno-rtti`.

`./install.sh` (`--binja-only`, `--ida-only`) fetches Qt when missing, builds the hosts it finds and copies the plugins.
For a dev loop, rebuild the tree and copy the module over the installed one (or symlink it once), then restart the host.

## Tests

```bash
ctest --test-dir build/none --output-on-failure     # QtTest suite
build/ida/rikugan_ida_tooltest [--dump DIR]          # IDA tools against a real database (needs idalib)
./ci-local.sh                                        # test tree + every host plugin available on this machine
```

- **Native suite** (`tests/native/`): `main.cpp` sets `HOME`, `RIKUGAN_HOME` and `TMPDIR` to a temporary directory,
  starts omc's mock OpenAI server (`$OMC_DIR/tests/e2e/mock_openai.py`) on a free port, registers it as the custom
  endpoint `mock/mock-1` and runs every `RK_TEST` class offscreen. Nothing touches the real `~/.rikugan`, `~/.omc` or
  credentials. `RK_KEEP_TMP=1` keeps the temporary home; the mock logs requests to `mock.jsonl` there.
  `FakeHost.h` is a `Host` with a few tools and main-thread marshalling through the Qt event loop; use it for chat,
  dispatcher, undo and mode tests. The mock decides each reply from the last message: prompt text such as
  `seq:[[tool, args], ...]` or `batch:[...]` scripts tool calls, `fail:<status>:<msg>` an HTTP error, `md:`/`chunks:`
  streamed answers (full list in the mock's docstring).
- **idalib tool test** (`tests/ida/`): opens a fresh database of `/bin/ls`, resolves `${...}` placeholders
  (a decompilable function, a named local, a caller …) and runs each case of `tool_cases.json` (`expect_contains`,
  `expect_error`), then checks that `/undo` restores a `rename_function`. Add a case for every new IDA tool.
- Binary Ninja tools have no headless harness; exercise them in the UI.

## Adding things

### A host tool

Add a `rk::ToolSpec` in the host's tool registration (`src/binja/tools/*.cpp`, `src/ida/host/tools/*.cpp`, or
`src/ida/sdk/*.cpp` when it subclasses SDK types). Template and flag rules: AGENTS.md → "How to Add a Tool". In short:

- pick the flags honestly (`Mutating`, `MainThread`, `RequiresDecompiler`, `Script`, `Exclusive`, `Blocking`);
- every `Mutating` tool is either in `MutationLog::reverseBuilders()` (with a pre-state reader in `capturePreState`,
  usually an `Internal` tool) or in `MutationLog::knownIrreversible()`;
- read arguments through `ToolCall` accessors, report bad input with `throw rk::ToolError(...)`, print addresses with
  `rk::hex`;
- on IDA, every SDK call on the main thread; never let an exception unwind through an IDA callback;
- if the tool lists or reads something expensive and stable, consider `ResultCache::cacheable`.

Then mention it in the host's `prompts/system/<host>/tool_usage.md` or `capabilities.md` if the model needs guidance,
add an IDA tooltest case, and update the tool reference in `webpage/docs.html`.

### A chat tool

Rikugan-side tools that are not host analysis (`ask_user`, `save_memory` …) live in `src/common/agent/ChatTools.cpp`.
Per-chat visibility goes in `chatToolVisible`; a tool shown only during a mode is toggled by `ModeDriver::setTools`.

### A skill

Create `skills/<slug>/SKILL.md` (and optional `references/*.md`):

```markdown
---
name: <slug>
description: One line the model and the composer show
mode: plan            # optional: plan | exploration
---
Task: <instruction for the agent>
```

`name` must equal the folder name (omc uses it as the skill id for `/<slug>` and `skill://<slug>`). Built-in skills
are embedded and synced into `~/.rikugan/omc/agent/skills/` when the version changes; user skills go there directly.
Add the slug to the table in `prompts/MANIFEST.md`.

### A prompt

Model-facing text lives in `prompts/` and is loaded with `embeddedText("prompts/...")`. Follow the conventions in
`prompts/MANIFEST.md` (one trailing newline, `{{name}}` placeholders only) and document the new file and its
placeholders there.

## Conventions

- C++20, Qt Widgets, `QT_NO_KEYWORDS` (`Q_SIGNALS`, `Q_SLOTS`, `Q_EMIT`). Shared code in `src/common`, host code only
  in `src/ida` / `src/binja`.
- The host UI thread never waits on omc, a session worker or a tool worker. UI updates go through `EventQueue` or a
  queued `invokeMethod` guarded by `QPointer`. No polling timers while idle; omc callbacks never block.
- IDA: Qt and STL headers before `ida/host/IdaSdk.h`. Binary Ninja: core API from any thread, UI and mutating calls on
  the main thread.
- Errors are reported, never swallowed. No stubs or placeholders.
- Headers document behaviour and threading; keep them in step with the code.

## Branches and commits

```
feat/my-thing  ─┐
fix/some-bug   ─┤──► dev ──► main
chore/deps     ─┘
```

- Branch off `dev`, open PRs against `dev`; `main` only takes PRs. Run `./ci-local.sh` before opening one.
- Commit prefix `fix(scope)`, `feat(scope)`, `refactor(scope)`, `security`, `docs`; scopes `ida`, `binja`, `agent`,
  `ui`, `omc`, `tools`, `build`. One logical change per commit.

## Releases

1. Merge `dev` → `main`.
2. Bump `project(rikugan VERSION x.y.z)` in `CMakeLists.txt` and `version` in `ida-plugin.json`.
3. `git tag vx.y.z && git push origin vx.y.z` — the release workflow checks that the tag matches both and publishes the
   GitHub release.

## Help

Issues: https://github.com/buzzer-re/Rikugan/issues
