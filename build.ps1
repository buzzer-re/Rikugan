# Builds the Rikugan plugins on Windows and packages them as release assets in dist\ (twin of build.sh):
#   dist\rikugan-<version>-<host>-windows-x86_64.zip (+ .sha256), host = ida | binja
# Each archive holds exactly what goes into the host's plugins folder:
#   ida:   rikugan\ida-plugin.json, rikugan\rikugan.dll, rikugan\libomc.dll
#   binja: rikugan.dll, rikugan\libomc.dll   (Binary Ninja loads every DLL at the top of plugins\ as a plugin)
# install.ps1 installs them from a release, or from this folder with -From dist.
#
#   .\build.ps1                            # IDA, and Binary Ninja when it is installed
#   .\build.ps1 -Sdk                       # both, without Binary Ninja installed (what CI and the release build)
#   .\build.ps1 -Target ida                # or binja
#   .\build.ps1 -OmcVersion latest         # libomc from omc's Windows release instead of ..\ohmyc
#
# Needs: Visual Studio 2022 or later with "Desktop development with C++" (MSVC, CMake, Ninja), git, 7-Zip or Windows'
# tar.exe (to unpack Qt), Binary Ninja installed for its plugin (its import libraries), and libomc, one of:
#   -OmcDir      omc's source (default ..\ohmyc), built through WSL (apt: mingw-w64 make gcc) or MSYS2 UCRT64 (gcc, make)
#   -OmcLibDir   a folder with a prebuilt libomc.dll and libomc.def (omc.h in ..\include)
#   -OmcVersion  an omc release (e.g. 19.1.1 or latest) whose Windows zip carries libomc; used when there is no source
# IDA needs no install to build: the SDK (third_party\ida-sdk) carries the import libraries for IDA and its Qt.
# -Sdk links Binary Ninja's plugin against stand-ins for its core and UI DLLs (cmake\host_stubs.cmake): it binds to the
# installed Binary Ninja's own when it loads, like a plugin built against an install.
param(
    [ValidateSet('both', 'binja', 'ida')][string]$Target = 'both',
    [switch]$Sdk,
    [string]$OmcDir = $env:OMC_DIR,
    [string]$OmcLibDir = $env:OMC_LIB_DIR,
    [string]$OmcVersion = $env:OMC_VERSION,
    [string]$BnInstallDir = $env:BN_INSTALL_DIR
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$Root = $PSScriptRoot
. (Join-Path $Root 'tools\vsenv.ps1')
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
Add-Type -AssemblyName System.IO.Compression.FileSystem

function Write-Step([string]$Msg) { Write-Host "== $Msg" -ForegroundColor Cyan }

# <home>\Qt\<version>\msvc2022_64, fetched when missing.
function Get-QtKit([string]$Version) {
    $dir = Join-Path $HOME "Qt\$Version\msvc2022_64"
    if (-not (Test-Path (Join-Path $dir 'lib\cmake\Qt6\Qt6Config.cmake'))) {
        & (Join-Path $Root 'tools\fetch_qt.ps1') $Version | Out-Null
    }
    return $dir
}

# The lib\ folder of omc's Windows release zip (verified against its .sha256), downloaded into build\omc-release\.
function Get-OmcRelease([string]$Version) {
    $repo = 'buzzer-re/Oh-My-C'
    if (-not $Version -or $Version -eq 'latest') {
        $rel = Invoke-RestMethod -UseBasicParsing -Uri "https://api.github.com/repos/$repo/releases/latest" -Headers @{ 'User-Agent' = 'rikugan-build' }
        $Version = ([string]$rel.tag_name).TrimStart('v')
    }
    $Version = $Version.TrimStart('v')
    $asset = "omc-$Version-windows-x86_64.zip"
    $dir = Join-Path $Root "build\omc-release\$Version"
    $lib = Join-Path $dir "omc-$Version-windows-x86_64\lib"
    if (Test-Path (Join-Path $lib 'libomc.dll')) { return $lib }
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    $zip = Join-Path $dir $asset
    $base = "https://github.com/$repo/releases/download/v$Version"
    Invoke-WebRequest -UseBasicParsing -Uri "$base/$asset" -OutFile $zip
    $want = ((Invoke-WebRequest -UseBasicParsing -Uri "$base/$asset.sha256").Content)
    if ($want -is [byte[]]) { $want = [Text.Encoding]::ASCII.GetString($want) }
    $want = ($want.Trim() -split '\s+')[0].ToLowerInvariant()
    $got = (Get-FileHash -Algorithm SHA256 -LiteralPath $zip).Hash.ToLowerInvariant()
    if ($want -ne $got) { throw "checksum mismatch for $asset" }
    Expand-Archive -LiteralPath $zip -DestinationPath $dir -Force
    if (-not (Test-Path (Join-Path $lib 'libomc.def'))) { throw "omc $Version's Windows release carries no libomc" }
    return $lib
}

function Get-Version {
    $m = Select-String -Path (Join-Path $Root 'CMakeLists.txt') -Pattern '^project\(rikugan VERSION ([0-9.]+)'
    return $m.Matches[0].Groups[1].Value
}

# dist\rikugan-<version>-<host>-windows-x86_64<suffix>.zip from a staged folder, with its .sha256
function New-Package([string]$HostName, [string]$Staged, [string]$Suffix = '') {
    $dist = Join-Path $Root 'dist'
    New-Item -ItemType Directory -Force -Path $dist | Out-Null
    $asset = "rikugan-$(Get-Version)-$HostName-windows-x86_64$Suffix.zip"
    $zip = Join-Path $dist $asset
    Remove-Item -LiteralPath $zip -ErrorAction SilentlyContinue
    [IO.Compression.ZipFile]::CreateFromDirectory($Staged, $zip)
    $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $zip).Hash.ToLowerInvariant()
    [IO.File]::WriteAllText("$zip.sha256", "$hash  $asset`n")
    Write-Host "Packaged dist\$asset"
}

# rikugan-<version>-<host>-windows-x86_64-symbols.zip: the plugin's PDB, to read a crash log's module+offset lines
# (next to rikugan.dll, it also names the functions in Rikugan's own crash logs)
function New-SymbolsPackage([string]$HostName, [string]$BuildDir) {
    $stage = Join-Path $Root "build\package\$HostName-symbols"
    Remove-Item -Recurse -Force -LiteralPath $stage -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force -Path $stage | Out-Null
    Copy-Item (Join-Path $BuildDir 'rikugan.pdb') $stage
    New-Package $HostName $stage '-symbols'
}

function New-Stage([string]$HostName) {
    $stage = Join-Path $Root "build\package\$HostName"
    Remove-Item -Recurse -Force -LiteralPath $stage -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force -Path (Join-Path $stage 'rikugan') | Out-Null
    return $stage
}

# Builds rikugan_<host> in build\<host> (build\<host>-sdk with -Sdk); returns the build folder.
function Invoke-HostBuild([string]$HostName, [string]$Qt, [string[]]$Extra) {
    $dir = Join-Path $Root ("build\$HostName" + $(if ($Sdk) { '-sdk' } else { '' }))
    $a = @('-S', $Root, '-B', $dir, '-G', 'Ninja', "-DRIKUGAN_HOST=$HostName",
           '-DCMAKE_BUILD_TYPE=RelWithDebInfo', '-DRIKUGAN_UPDATES=ON', "-DCMAKE_PREFIX_PATH=$Qt")
    if ($Sdk) { $a += '-DRIKUGAN_SDK_ONLY=ON' }
    # both, so a tree switches between a prebuilt libomc and the source build
    if ($script:OmcLibDir) { $a += @("-DOMC_DIR=$script:OmcDir", "-DOMC_LIB_DIR=$script:OmcLibDir") } else { $a += @("-DOMC_DIR=$script:OmcDir", '-DOMC_LIB_DIR=') }
    # shown, not returned: the caller gets the folder only
    Invoke-Native cmake ($a + $Extra) | Out-Host
    Invoke-Native cmake @('--build', $dir, '--target', "rikugan_$HostName") | Out-Host
    return $dir
}

# ---- libomc ----
if (-not $OmcDir) { $OmcDir = Join-Path (Split-Path $Root -Parent) 'ohmyc' }
if (-not $OmcLibDir) {
    if ($OmcVersion) {
        $OmcLibDir = Get-OmcRelease $OmcVersion
    } elseif (-not (Test-Path (Join-Path $OmcDir 'include\omc.h'))) {
        Write-Step "no omc source at ${OmcDir}: using libomc from omc's latest Windows release"
        $OmcLibDir = Get-OmcRelease 'latest'
    }
}
if ($OmcLibDir) { Write-Step "libomc: $OmcLibDir" } else { Write-Step "libomc: built from $OmcDir" }

Import-VsDevEnv
Push-Location $Root
try { Invoke-Native git @('submodule', 'update', '--init', '--recursive', 'third_party') } finally { Pop-Location }

$built = $false

if ($Target -in 'both', 'binja') {
    $bn = @($BnInstallDir, "$env:LOCALAPPDATA\Programs\Vector35\BinaryNinja", "$env:ProgramFiles\Vector35\BinaryNinja") |
        Where-Object { $_ -and (Test-Path (Join-Path $_ 'binaryninjacore.dll')) } | Select-Object -First 1
    if ($bn -or $Sdk) {
        if ($Sdk) { Write-Step 'Binary Ninja (no install: stand-ins for its DLLs)'; $extra = @() }
        else { Write-Step "Binary Ninja at $bn"; $extra = @("-DBN_INSTALL_DIR=$bn") }
        $dir = Invoke-HostBuild 'binja' (Get-QtKit '6.11.1') $extra
        $stage = New-Stage 'binja'
        Copy-Item (Join-Path $dir 'rikugan.dll') $stage
        Copy-Item (Join-Path $dir 'libomc.dll') (Join-Path $stage 'rikugan')
        New-Package 'binja' $stage
        New-SymbolsPackage 'binja' $dir
        $built = $true
    } elseif ($Target -eq 'binja') {
        throw 'Binary Ninja not found (pass -BnInstallDir): its plugin links against the installed core and UI.'
    } else {
        Write-Step 'Binary Ninja: not found, skipped (pass -BnInstallDir)'
    }
}

if ($Target -in 'both', 'ida') {
    Write-Step 'IDA Pro'
    $dir = Invoke-HostBuild 'ida' (Get-QtKit '6.8.2') @()
    $stage = New-Stage 'ida'
    Copy-Item (Join-Path $Root 'ida-plugin.json'), (Join-Path $dir 'rikugan.dll'), (Join-Path $dir 'libomc.dll') (Join-Path $stage 'rikugan')
    New-Package 'ida' $stage
    New-SymbolsPackage 'ida' $dir
    $built = $true
}

if (-not $built) { throw 'No host built.' }
Write-Host 'Done. Install the packages with .\install.ps1 -From dist'
