# fetch_qt.ps1 <version> [dest]   e.g.  tools\fetch_qt.ps1 6.11.1   (Binary Ninja)   tools\fetch_qt.ps1 6.8.2   (IDA 9.3)
# Windows twin of fetch_qt.sh: the official Qt desktop build for MSVC 2022 x64 (qtbase only) from download.qt.io,
# checked against the published SHA-1, unpacked into <dest>\<version>\msvc2022_64 (default ~\Qt). Only headers,
# moc/rcc/uic, import libraries and CMake files are used at build time: the plugin binds to the host's own Qt at
# runtime (IDA: the SDK's import libraries for its namespaced Qt). Unpacks with 7-Zip when installed, else with
# Windows' tar.exe. Prints the kit directory.
param(
    [Parameter(Mandatory = $true, Position = 0)][string]$Version,
    [Parameter(Position = 1)][string]$Dest = (Join-Path $HOME 'Qt')
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12

$Kit = 'msvc2022_64'
$Pkg = 'win64_msvc2022_64'
$NoDot = $Version -replace '\.', ''
$Repo = 'https://download.qt.io/online/qtsdkrepository/windows_x86/desktop'
$Out = Join-Path (Join-Path $Dest $Version) $Kit

function Get-Text([string]$Url) {
    try { $c = (Invoke-WebRequest -UseBasicParsing -Uri $Url).Content } catch { return $null }
    # a .sha1 file comes as application/octet-stream, which Invoke-WebRequest returns as bytes
    if ($c -is [byte[]]) { return [Text.Encoding]::ASCII.GetString($c) }
    return $c
}

# Qt moved the Windows packages of newer releases into a per-kit folder (qt6_6111/qt6_6111_msvc2022_64/);
# older ones (6.8) list every kit in qt6_682/qt6_682/
$Base = $null
$Listing = $null
foreach ($sub in @("qt6_${NoDot}_$Kit", "qt6_$NoDot")) {
    $url = "$Repo/qt6_$NoDot/$sub/qt.qt6.$NoDot.$Pkg/"
    $Listing = Get-Text $url
    if ($Listing) { $Base = $url; break }
}
if (-not $Base) { throw "fetch_qt.ps1: no Qt $Version package for $Kit under $Repo/qt6_$NoDot/" }

$archives = [regex]::Matches($Listing, 'href="([^"]*qtbase[^"]*\.7z)"') | ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique
if (-not $archives) { throw "fetch_qt.ps1: no qtbase archive at $Base" }

$SevenZip = @(
    (Get-Command 7z.exe -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source -First 1),
    (Join-Path $env:ProgramFiles '7-Zip\7z.exe')
) | Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1

$Tmp = Join-Path ([IO.Path]::GetTempPath()) ("rk-qt-" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $Tmp, $Out | Out-Null
try {
    foreach ($a in $archives) {
        $file = Join-Path $Tmp $a
        Invoke-WebRequest -UseBasicParsing -Uri "$Base$a" -OutFile $file
        $want = ((Get-Text "$Base$a.sha1") -split '\s+')[0]
        $got = (Get-FileHash -Algorithm SHA1 -Path $file).Hash
        if (-not $want -or $want.ToLowerInvariant() -ne $got.ToLowerInvariant()) { throw "fetch_qt.ps1: SHA-1 mismatch for $a" }
        if ($SevenZip) {
            & $SevenZip x -y "-o$Out" $file | Out-Null
        } else {
            & "$env:SystemRoot\System32\tar.exe" -xf $file -C $Out
        }
        if ($LASTEXITCODE -ne 0) { throw "fetch_qt.ps1: could not unpack $a (exit $LASTEXITCODE)" }
    }
} finally {
    Remove-Item -Recurse -Force -Path $Tmp -ErrorAction SilentlyContinue
}

$moc = Join-Path $Out 'bin\moc.exe'
if (-not (Test-Path (Join-Path $Out 'lib\cmake\Qt6\Qt6Config.cmake')) -or -not (Test-Path $moc)) {
    throw "fetch_qt.ps1: the Qt archive did not unpack into $Out (no lib\cmake\Qt6 or bin\moc.exe)"
}
& $moc -v | Out-Null
if ($LASTEXITCODE -ne 0) { throw "fetch_qt.ps1: $moc does not run (is the Visual C++ runtime installed?)" }
Write-Output $Out
