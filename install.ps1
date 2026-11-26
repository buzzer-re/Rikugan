<#
.SYNOPSIS
    Rikugan installer for Windows: installs a release's prebuilt plugins for IDA Pro and Binary Ninja.

.DESCRIPTION
    The twin of install.sh. Downloads rikugan-<version>-<host>-windows-x86_64.zip for each host found, checks it against
    its .sha256 and unpacks it into the host's plugins folder:
        Binary Ninja  <BN user dir>\plugins\rikugan.dll, with libomc.dll in plugins\rikugan\
        IDA Pro       <IDA user dir>\plugins\rikugan\ (ida-plugin.json, rikugan.dll, libomc.dll)
    -From installs the packages of a local build instead (.\build.ps1 writes them to dist\). A Rikugan 1.x (Python)
    install of the same host is moved to ~\.rikugan-1.x-backup\ first (asks, unless -Yes).

    Works in Windows PowerShell 5.1 and PowerShell 7:
        irm https://raw.githubusercontent.com/buzzer-re/Rikugan/main/install.ps1 | iex
        & ([scriptblock]::Create((irm https://raw.githubusercontent.com/buzzer-re/Rikugan/main/install.ps1))) -Target ida
        .\install.ps1 -From dist

.PARAMETER Target
    both (default: every host found), binja or ida. Also $env:RIKUGAN_TARGET.
.PARAMETER Version
    The release to install, e.g. 2.0.0 (default: the latest release). Also $env:RIKUGAN_VERSION.
.PARAMETER From
    A folder of packages built with build.ps1 to install instead of a release. Also $env:RIKUGAN_FROM.
.PARAMETER BnPluginsDir
    Binary Ninja's plugins folder (default: $env:BN_USER_DIRECTORY or %APPDATA%\Binary Ninja, + \plugins).
.PARAMETER IdaPluginsDir
    IDA's plugins folder (default: the first folder of $env:IDAUSR or %APPDATA%\Hex-Rays\IDA Pro, + \plugins).
.PARAMETER Yes
    Never ask: moves a Rikugan 1.x install aside without asking.
#>
[CmdletBinding()]
param(
    [ValidateSet('', 'both', 'binja', 'ida')][string]$Target = '',
    [string]$Version = '',
    [string]$From = '',
    [string]$BnPluginsDir = '',
    [string]$IdaPluginsDir = '',
    [switch]$Yes
)

# `irm ... | iex` runs this script in the caller's own scope: everything that changes the session (strict mode, error
# preference) is set inside Invoke-RikuganInstall, so the user's PowerShell is left as it was.

function Write-Step([string]$Message) { Write-Host "rikugan-install: $Message" }

function Stop-Install([string]$Message) {
    Write-Host "rikugan-install: error: $Message" -ForegroundColor Red
    throw "rikugan-install: $Message"
}

function Get-Setting([string]$Value, [string]$EnvName, [string]$Default) {
    if ($Value) { return $Value }
    $e = [Environment]::GetEnvironmentVariable($EnvName)
    if ($e) { return $e }
    return $Default
}

function Get-Download([string]$Url, [string]$OutFile) {
    try {
        $old = $ProgressPreference
        $ProgressPreference = 'SilentlyContinue'
        Invoke-WebRequest -Uri $Url -OutFile $OutFile -UseBasicParsing
        $ProgressPreference = $old
    } catch {
        Stop-Install "download failed: $Url ($($_.Exception.Message))"
    }
}

function Get-LatestVersion([string]$Repo) {
    try {
        $rel = Invoke-RestMethod -Uri "https://api.github.com/repos/$Repo/releases/latest" -UseBasicParsing -Headers @{ 'User-Agent' = 'rikugan-install' }
    } catch {
        Stop-Install "could not find the latest release of $Repo ($($_.Exception.Message))"
    }
    $tag = [string]$rel.tag_name
    if (-not $tag) { Stop-Install "could not find the latest release of $Repo (is it published?)" }
    return $tag.TrimStart('v')
}

function Test-Checksum([string]$File, [string]$SumFile, [string]$Asset) {
    $want = ((Get-Content -LiteralPath $SumFile -Raw).Trim() -split '\s+')[0].ToLowerInvariant()
    $got = (Get-FileHash -LiteralPath $File -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($got -ne $want) { Stop-Install "checksum mismatch for $Asset (expected $want, got $got); nothing was installed" }
}

function Confirm-Step([string]$Question) {
    if ($Yes) { return $true }
    return (Read-Host "$Question [Y/n]") -notmatch '^\s*(n|no)\s*$'
}

# A Rikugan 1.x file or folder goes to ~\.rikugan-1.x-backup\<host>\<time>\.
function Move-Aside([string]$Path, [string]$HostName) {
    $dest = Join-Path $HOME (".rikugan-1.x-backup\$HostName\" + (Get-Date -Format 'yyyyMMdd-HHmmss'))
    New-Item -ItemType Directory -Force -Path $dest | Out-Null
    Move-Item -LiteralPath $Path -Destination $dest
    Write-Step "moved $Path to $dest"
}

# The unpacked package of <host>: from the release (verified) or from the -From folder.
function Get-Package([string]$HostName, [string]$Tmp) {
    $from = Get-Setting $From 'RIKUGAN_FROM' ''
    if ($from) {
        $zip = Get-ChildItem -LiteralPath $from -Filter "rikugan-*-$HostName-windows-x86_64.zip" -ErrorAction SilentlyContinue |
            Sort-Object LastWriteTime -Descending | Select-Object -First 1
        if (-not $zip) { Stop-Install "no rikugan-*-$HostName-windows-x86_64.zip in $from (.\build.ps1 makes them)" }
        $file = $zip.FullName
        if (Test-Path -LiteralPath "$file.sha256") { Test-Checksum $file "$file.sha256" $zip.Name }
    } else {
        $repo = Get-Setting '' 'RIKUGAN_REPO' 'buzzer-re/Rikugan'
        $base = Get-Setting '' 'RIKUGAN_DOWNLOAD_URL' "https://github.com/$repo/releases/download"
        if (-not $script:ver) {
            $script:ver = Get-Setting $Version 'RIKUGAN_VERSION' ''
            if (-not $script:ver) { $script:ver = Get-LatestVersion $repo }
            $script:ver = $script:ver.TrimStart('v')
        }
        $asset = "rikugan-$($script:ver)-$HostName-windows-x86_64.zip"
        $file = Join-Path $Tmp $asset
        Write-Step "downloading $asset"
        Get-Download "$base/v$($script:ver)/$asset" $file
        Get-Download "$base/v$($script:ver)/$asset.sha256" "$file.sha256"
        Test-Checksum $file "$file.sha256" $asset
    }
    $out = Join-Path $Tmp $HostName
    Expand-Archive -LiteralPath $file -DestinationPath $out -Force
    return $out
}

function Install-Files([string]$Package, [string]$Dest) {
    New-Item -ItemType Directory -Force -Path $Dest | Out-Null
    foreach ($f in Get-ChildItem -LiteralPath $Package -Recurse -File) {
        $rel = $f.FullName.Substring($Package.Length).TrimStart('\')
        $to = Join-Path $Dest $rel
        New-Item -ItemType Directory -Force -Path (Split-Path $to -Parent) | Out-Null
        try {
            Copy-Item -LiteralPath $f.FullName -Destination $to -Force
        } catch {
            Stop-Install "could not write $to ($($_.Exception.Message)); close the host and run the installer again"
        }
    }
}

function Invoke-RikuganInstall {
    $ErrorActionPreference = 'Stop'
    Set-StrictMode -Version 2.0
    try { [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12 } catch { }
    $target = Get-Setting $Target 'RIKUGAN_TARGET' 'both'
    $script:ver = ''
    $tmp = Join-Path ([IO.Path]::GetTempPath()) ('rikugan-install-' + [Guid]::NewGuid().ToString('N').Substring(0, 8))
    New-Item -ItemType Directory -Path $tmp | Out-Null
    $installed = $false
    try {
        if ($target -in 'both', 'binja') {
            $userDir = if ($env:BN_USER_DIRECTORY) { $env:BN_USER_DIRECTORY } else { Join-Path $env:APPDATA 'Binary Ninja' }
            $plugins = if ($BnPluginsDir) { $BnPluginsDir } else { Join-Path $userDir 'plugins' }
            if ($BnPluginsDir -or (Test-Path -LiteralPath $userDir)) {
                Write-Step "Binary Ninja: $plugins"
                $pkg = Get-Package 'binja' $tmp
                # 1.x was the folder plugins\rikugan (a Python plugin); 2.x keeps only libomc.dll there
                $libDir = Join-Path $plugins 'rikugan'
                if ((Test-Path -LiteralPath $libDir) -and (Get-ChildItem -Force -LiteralPath $libDir | Where-Object { $_.Name -ne 'libomc.dll' })) {
                    if (-not (Confirm-Step "$libDir holds Rikugan 1.x (or a checkout), which would load next to 2.x. Move it to ~\.rikugan-1.x-backup?")) {
                        Stop-Install "$libDir is in the way"
                    }
                    Move-Aside $libDir 'binaryninja'
                }
                Install-Files $pkg $plugins
                Write-Step "installed $plugins\rikugan.dll"
                $installed = $true
            } elseif ($target -eq 'binja') {
                Stop-Install "no Binary Ninja user folder at $userDir (run Binary Ninja once, or pass -BnPluginsDir)"
            } else {
                Write-Step "Binary Ninja: no user folder at $userDir, skipped"
            }
        }
        if ($target -in 'both', 'ida') {
            $userDir = if ($env:IDAUSR) { ($env:IDAUSR -split ';')[0] } else { Join-Path $env:APPDATA 'Hex-Rays\IDA Pro' }
            $plugins = if ($IdaPluginsDir) { $IdaPluginsDir } else { Join-Path $userDir 'plugins' }
            if ($IdaPluginsDir -or (Test-Path -LiteralPath $userDir)) {
                Write-Step "IDA Pro: $plugins"
                $pkg = Get-Package 'ida' $tmp
                $dest = Join-Path $plugins 'rikugan'
                $loader = Join-Path $plugins 'rikugan_plugin.py'
                $ours = (Test-Path -LiteralPath (Join-Path $dest 'ida-plugin.json')) -and (Test-Path -LiteralPath (Join-Path $dest 'rikugan.dll'))
                if ((Test-Path -LiteralPath $loader) -or ((Test-Path -LiteralPath $dest) -and -not $ours)) {
                    if (-not (Confirm-Step 'IDA has Rikugan 1.x (rikugan_plugin.py / plugins\rikugan), which would load next to 2.x. Move it to ~\.rikugan-1.x-backup?')) {
                        Stop-Install 'Rikugan 1.x is in the way'
                    }
                    if (Test-Path -LiteralPath $loader) { Move-Aside $loader 'ida' }
                    if ((Test-Path -LiteralPath $dest) -and -not $ours) { Move-Aside $dest 'ida' }
                }
                Install-Files $pkg $plugins
                Write-Step "installed $dest"
                $installed = $true
            } elseif ($target -eq 'ida') {
                Stop-Install "no IDA user folder at $userDir (run IDA once, or pass -IdaPluginsDir)"
            } else {
                Write-Step "IDA Pro: no user folder at $userDir, skipped"
            }
        }
        if (-not $installed) { Stop-Install 'no host found (pass -Target with -BnPluginsDir or -IdaPluginsDir)' }
        Write-Step 'done. Restart the host, then connect a provider from the model picker (Ctrl+M) or Settings -> Provider.'
        Write-Step 'Rikugan installs later releases itself, when you agree (Settings -> Behavior -> Updates).'
    } finally {
        Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
    }
}

Invoke-RikuganInstall
