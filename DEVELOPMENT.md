# Development Guide

> Coding agents: read [AGENTS.md](AGENTS.md). Internals: [ARCHITECTURE.md](ARCHITECTURE.md).

Rikugan is a native C++20 / Qt Widgets plugin for IDA Pro and Binary Ninja; its agent is libomc, linked in-process.
This guide covers the development setup, builds, tests and the contribution workflow. (Rikugan 1.x was a Python
plugin; it lives only in the git history.)

## Prerequisites

- Linux, macOS or Windows 10/11 x64.
- Linux/macOS: CMake ≥ 3.25, Ninja (or make), a C++20 compiler, OpenSSL 3 and zlib headers, `curl` and `7z` (p7zip)
  for `tools/fetch_qt.sh`. Debian/Ubuntu: `build-essential cmake ninja-build libssl-dev zlib1g-dev p7zip-full curl`.
- Windows: Visual Studio 2022 or later with "Desktop development with C++" (MSVC, its CMake and Ninja), git, and
  7-Zip or Windows' `tar.exe` for `tools/fetch_qt.ps1`. libomc.dll is built by omc's own MinGW-w64 Makefile: through
  WSL (`sudo apt install mingw-w64 make gcc`) or MSYS2 UCRT64 (`pacman -S mingw-w64-ucrt-x86_64-gcc make`), or taken
  prebuilt from omc's Windows release (`build.ps1 -OmcVersion latest`).
- `python3` for the test suite (omc's mock model server).
- The hosts you build for: IDA Pro 9.3 (default `~/ida-pro-9.3`, override `IDA_DIR`; on Windows the SDK's import
  libraries are enough to build) and/or Binary Ninja (default `~/binaryninja`, Windows
  `%LOCALAPPDATA%\Programs\Vector35\BinaryNinja`, override `BN_INSTALL_DIR`).

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
tools\fetch_qt.ps1 6.11.1    # Windows (msvc2022_64 kit); same versions
```

`<kit>` is `gcc_arm64`, `gcc_64`, `macos` or `msvc2022_64`. On Windows the IDA tree links IDA's namespaced Qt through
the import libraries in the SDK (`third_party/ida-sdk/src/lib/x64_win_qt`).

## Build trees

One host per tree; `src/common` is compiled into each tree with that tree's Qt.

```bash
cmake -S . -B build/none  -G Ninja -DRIKUGAN_HOST=none  -DCMAKE_PREFIX_PATH=~/Qt/6.11.1/<kit> && cmake --build build/none
cmake -S . -B build/binja -G Ninja -DRIKUGAN_HOST=binja -DCMAKE_PREFIX_PATH=~/Qt/6.11.1/<kit> && cmake --build build/binja
cmake -S . -B build/ida   -G Ninja -DRIKUGAN_HOST=ida   -DCMAKE_PREFIX_PATH=~/Qt/6.8.2/<kit>  && cmake --build build/ida
```

On Windows run the same from an x64 Visual Studio developer prompt (`tools\vsenv.ps1`'s `Import-VsDevEnv` sets one up
in PowerShell). Without `CMAKE_BUILD_TYPE` every tree is `RelWithDebInfo`; on MSVC every configuration links the
release C runtime and release Qt, since that is what the hosts load.

| Tree | Output |
|---|---|
| `build/none` | `rikugan_tests` (no host) |
| `build/binja` | `librikugan.so` / `.dylib`; Windows `rikugan.dll` + `libomc.dll` |
| `build/ida` | `rikugan.so` / `.dylib`; Windows `rikugan.dll` + `libomc.dll`; plus `rikugan_ida_tooltest` |

Cache variables: `OMC_DIR` (default `../ohmyc`), `BN_INSTALL_DIR`, `IDA_DIR`, `RIKUGAN_BUILD_TESTS` (on for `none`);
Windows also `OMC_LIB_DIR` (a prebuilt libomc: a folder with `libomc.dll` and `libomc.def`, `omc.h` in `../include`) and
`OMC_BUILD_WITH` (`native` or `wsl`; empty picks MinGW `make` + `gcc` on PATH, else WSL).

- Linux/macOS: each build runs `cmake/build_omc.sh`: omc's own `make` builds its objects incrementally, then they are
  prelinked into `build/<tree>/omc/omc-lib.o` with only `omc_*` symbols global. Changes in `../ohmyc` are picked up on
  the next build.
- Windows: each build runs `cmake/build_omc_win.cmake`: omc's `make WINDOWS=1` builds `libomc.dll` (MinGW-w64, through
  WSL or MSYS2) unless `OMC_LIB_DIR` names a prebuilt one, then `lib.exe` makes an MSVC import library from its
  `libomc.def` and the DLL is copied next to the tree's modules. The plugins delay-load it; at init
  `rk::loadOmcLibrary` (`omc/OmcLibrary.h`) loads it by full path from the plugin's folder or its `rikugan\`
  subfolder and checks the API version.
- `cmake/embed.cmake` compiles every file under `skills/` and `prompts/` into the plugin (CRLF line ends of a Windows
  checkout are normalized); editing them needs a rebuild, not a reinstall of loose files.
- Files under `src/ida/sdk/` subclass SDK types and are compiled with `-fno-rtti` (GCC/clang; MSVC emits RTTI in the
  module that uses it).

## Packages and installing

`./build.sh` (Linux, macOS; `--binja-only`, `--ida-only`) and `.\build.ps1` (Windows; `-Target`, `-OmcDir`,
`-OmcLibDir`, `-OmcVersion`) fetch Qt when missing, build the hosts they can and write the release assets to `dist/`:

| Asset | Content (unpacked into the host's plugins folder) |
|---|---|
| `rikugan-<v>-ida-<os>-<arch>.tar.gz` | `rikugan.so` / `rikugan.dylib` |
| `rikugan-<v>-binja-<os>-<arch>.tar.gz` | `librikugan.so` / `librikugan.dylib` |
| `rikugan-<v>-ida-windows-x86_64.zip` | `rikugan\ida-plugin.json`, `rikugan\rikugan.dll`, `rikugan\libomc.dll` |
| `rikugan-<v>-binja-windows-x86_64.zip` | `rikugan.dll`, `rikugan\libomc.dll` (Binary Ninja loads every DLL at the top of `plugins\` as a plugin) |

`<os>-<arch>` is `linux-x86_64`, `linux-arm64`, `macos-arm64`, `macos-x86_64`. Each asset has a `<asset>.sha256`
(`<hash>  <asset>`). `install.sh` / `install.ps1` download `v<version>/<asset>` and its `.sha256` from the GitHub
release (`RIKUGAN_DOWNLOAD_URL` overrides the base, `RIKUGAN_REPO` the repository), or take them from a folder:
`./install.sh --from dist`, `.\install.ps1 -From dist`. The Binary Ninja package only loads in the Binary Ninja whose
API revision it was built against (`third_party/binaryninja-api`).

For a dev loop, rebuild the tree and copy the module over the installed one (or symlink it once), then restart the host.

## Tests

```bash
ctest --test-dir build/none --output-on-failure     # QtTest suite
build/ida/rikugan_ida_tooltest [--dump DIR] [--binary FILE]   # IDA tools against a real database (needs idalib)
./ci-local.sh                                        # test tree + every host plugin available on this machine
.\ci-local.ps1                                       # Windows: test tree + build.ps1
```

- **Native suite** (`tests/native/`): `main.cpp` sets `HOME`, `RIKUGAN_HOME` and `TMPDIR` (Windows: `USERPROFILE` too)
  to a temporary directory through `rk::setEnv`, so libomc's own C runtime on Windows sees them as well, starts omc's
  mock OpenAI server (`$OMC_DIR/tests/e2e/mock_openai.py`, on the Python CMake found: `RK_PYTHON`) on a free port,
  registers it as the custom endpoint `mock/mock-1` and runs every `RK_TEST` class offscreen. Nothing touches the real
  `~/.rikugan`, `~/.omc` or credentials. `RK_KEEP_TMP=1` keeps the temporary home; the mock logs requests to
  `mock.jsonl` there.
  `FakeHost.h` is a `Host` with a few tools and main-thread marshalling through the Qt event loop; use it for chat,
  dispatcher, undo and mode tests. The mock decides each reply from the last message: prompt text such as
  `seq:[[tool, args], ...]` or `batch:[...]` scripts tool calls, `fail:<status>:<msg>` an HTTP error, `md:`/`chunks:`
  streamed answers (full list in the mock's docstring).
- **idalib tool test** (`tests/ida/`): opens a fresh database of `/bin/ls` (Windows: `notepad.exe`; `--binary` picks
  another), resolves `${...}` placeholders (a decompilable function, a named local, a caller, the database's type
  library …) and runs each case of `tool_cases.json` (`expect_contains`, `expect_error`), then checks that `/undo`
  restores a `rename_function`. Add a case for every new IDA tool. On Windows run it with IDA's folder on `PATH`.
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
- Portable code: platform calls stay behind `#ifdef _WIN32` in the few files that need them (`util/Env`,
  `omc/OmcLibrary`, `mcp/McpClient`, `util/Files`). Environment variables omc must see go through `rk::setEnv` /
  `rk::unsetEnv`, the home folder through `rk::homeDir()` (omc's rule: `$HOME`, else the platform's home).
- Errors are reported, never swallowed. No stubs or placeholders.
- Headers document behaviour and threading; keep them in step with the code.

## Branches and commits

```
feat/my-thing  ─┐
fix/some-bug   ─┤──► dev ──► main
chore/deps     ─┘
```

- Branch off `dev`, open PRs against `dev`; `main` only takes PRs. Run `./ci-local.sh` (Windows: `.\ci-local.ps1`)
  before opening one.
- Commit prefix `fix(scope)`, `feat(scope)`, `refactor(scope)`, `security`, `docs`; scopes `ida`, `binja`, `agent`,
  `ui`, `omc`, `tools`, `build`. One logical change per commit.

## Releases

1. Merge `dev` → `main`.
2. Bump `project(rikugan VERSION x.y.z)` in `CMakeLists.txt` and `version` in `ida-plugin.json`.
3. `git tag vx.y.z && git push origin vx.y.z` — the release workflow checks that the tag matches both and publishes the
   GitHub release.
4. The release carries the packages `build.sh` / `build.ps1` write to `dist/` for every platform, each with its
   `.sha256` (names and contents: "Packages and installing" above); the installers download exactly those.

## Help

Issues: https://github.com/buzzer-re/Rikugan/issues
