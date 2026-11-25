#!/bin/sh
# Release notes for Rikugan v<version>, used by .github/workflows/release.yml; run it locally to preview.
# docs/releases/v<version>.md, when it exists, is the body's opening (what changed); the install section and the
# asset table below follow it.
# Usage: build-tools/release_notes.sh <version>
set -eu
cd "$(dirname "$0")/.."
V=${1:?usage: release_notes.sh <version>}

if [ -f "docs/releases/v$V.md" ]; then
    cat "docs/releases/v$V.md"
    echo
else
    echo "Rikugan v$V."
    echo
fi

cat <<EOF
## Install

The installers find IDA Pro 9.3 and Binary Ninja on this computer, download the plugin for each from this release,
check it against its \`.sha256\` and put it in the host's plugins folder. Restart the host afterwards.

Linux and macOS:

\`\`\`sh
curl -fsSL https://raw.githubusercontent.com/buzzer-re/Rikugan/main/install.sh | RIKUGAN_VERSION=$V bash
\`\`\`

Windows (PowerShell):

\`\`\`powershell
\$env:RIKUGAN_VERSION = '$V'; irm https://raw.githubusercontent.com/buzzer-re/Rikugan/main/install.ps1 | iex
\`\`\`

Rikugan checks for a newer release once a day and asks before it installs one; Settings › Behavior › Check for updates
turns that off.

| Package | Host | Runs on |
|---|---|---|
| \`rikugan-$V-ida-linux-x86_64.tar.gz\`, \`rikugan-$V-binja-linux-x86_64.tar.gz\` | IDA Pro 9.3, Binary Ninja | Linux x86_64, glibc 2.35+ and OpenSSL 3 (Ubuntu 22.04+, Debian 12+, Fedora 36+) |
| \`rikugan-$V-ida-linux-arm64.tar.gz\`, \`rikugan-$V-binja-linux-arm64.tar.gz\` | IDA Pro 9.3, Binary Ninja | Linux arm64, glibc 2.39+ and OpenSSL 3 (Ubuntu 24.04+, Debian 13+, Fedora 40+) |
| \`rikugan-$V-ida-macos-arm64.tar.gz\`, \`rikugan-$V-binja-macos-arm64.tar.gz\` | IDA Pro 9.3, Binary Ninja | macOS on Apple silicon (OpenSSL is built in) |
| \`rikugan-$V-ida-macos-x86_64.tar.gz\`, \`rikugan-$V-binja-macos-x86_64.tar.gz\` | IDA Pro 9.3, Binary Ninja | macOS on Intel |
| \`rikugan-$V-ida-windows-x86_64.zip\`, \`rikugan-$V-binja-windows-x86_64.zip\` | IDA Pro 9.3, Binary Ninja | Windows 10 and 11, x64 |

Each package holds exactly what goes into the host's plugins folder. The \`-symbols\` packages hold the debug
information the plugins were stripped of (a \`.debug\` file, a \`.dSYM\` bundle or a \`.pdb\`), to read a crash log's
\`module+offset\` lines. \`SHA256SUMS\` lists every file; by hand:

\`\`\`sh
sha256sum -c SHA256SUMS --ignore-missing      # macOS: shasum -a 256 -c SHA256SUMS --ignore-missing
tar -xzf rikugan-$V-binja-linux-x86_64.tar.gz -C ~/.binaryninja/plugins
\`\`\`

The macOS plugins are not notarized: a copy downloaded with a browser needs \`xattr -d com.apple.quarantine <file>\`
once (the installers and the updater download with curl, which marks nothing).
EOF
