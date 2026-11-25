# CI and release steps on Windows (MSVC), the twin of build-tools/ci.sh. libomc.dll is built before, in Git Bash
# (build-tools/ci.sh libomc), from the omc source build-tools/ci.sh omc checked out.
#
#   build-tools\ci.ps1 test       the native test suite (build\none): zero warnings, then ctest
#   build-tools\ci.ps1 plugins    both plugins without Binary Ninja installed (build.ps1 -Sdk): zero warnings, packaged
#                                 into dist\ as the release ships them
#   build-tools\ci.ps1 smoke      each package in dist\: its files, what its DLLs import and export
param(
    [Parameter(Mandatory = $true, Position = 0)][ValidateSet('test', 'plugins', 'smoke')][string]$Step,
    [string]$OmcDir = $env:OMC_DIR,
    [string]$Python = $env:PYTHON
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$Root = Split-Path $PSScriptRoot -Parent
. (Join-Path $Root 'tools\vsenv.ps1')
Add-Type -AssemblyName System.IO.Compression.FileSystem

if (-not $OmcDir) { $OmcDir = Join-Path $Root 'build\ohmyc' }
$OmcLib = Join-Path $OmcDir 'build\windows'
$Version = (Select-String -Path (Join-Path $Root 'CMakeLists.txt') -Pattern '^project\(rikugan VERSION ([0-9.]+)').Matches[0].Groups[1].Value

function Stop-Ci([string]$Message) { throw "ci: $Message" }

# The output of a build, kept in <log>: it must hold no compiler or linker warning.
function Assert-NoWarnings([string]$Log) {
    $hits = @(Select-String -Path $Log -Pattern '(^|[ :])warning( [A-Z]+[0-9]+)?:' -CaseSensitive:$false)
    if ($hits.Count -gt 0) {
        $hits | Select-Object -First 30 | ForEach-Object { Write-Host $_.Line }
        Stop-Ci "$($hits.Count) warning(s) in the build; it must have none"
    }
}

function Invoke-Logged([string]$Log, [scriptblock]$Block) {
    New-Item -ItemType Directory -Force -Path (Split-Path $Log -Parent) | Out-Null
    & $Block 2>&1 | Tee-Object -FilePath $Log | Out-Host
    if ($LASTEXITCODE -ne 0) { Stop-Ci "the build failed (see $Log)" }
}

function Invoke-Test {
    if (-not (Test-Path (Join-Path $OmcLib 'libomc.dll'))) { Stop-Ci "no libomc.dll in $OmcLib (build-tools/ci.sh libomc, in Git Bash)" }
    if (-not $Python) {
        $Python = Get-Command python.exe, python3.exe -ErrorAction SilentlyContinue |
            Where-Object { $_.Source -notmatch 'WindowsApps' } | Select-Object -First 1 -ExpandProperty Source
    }
    if (-not $Python) { Stop-Ci 'no Python 3 (the tests run omc''s mock model with it)' }
    $qt = Join-Path $HOME 'Qt\6.11.1\msvc2022_64'
    if (-not (Test-Path (Join-Path $qt 'lib\cmake\Qt6\Qt6Config.cmake'))) { & (Join-Path $Root 'tools\fetch_qt.ps1') 6.11.1 | Out-Null }
    $dir = Join-Path $Root 'build\none'
    Invoke-Native cmake @('-S', $Root, '-B', $dir, '-G', 'Ninja', '-DRIKUGAN_HOST=none', "-DOMC_DIR=$OmcDir",
                          "-DOMC_LIB_DIR=$OmcLib", "-DCMAKE_PREFIX_PATH=$qt", "-DPython3_EXECUTABLE=$Python")
    $log = Join-Path $Root 'build\ci-test-build.log'
    Invoke-Logged $log { cmake --build $dir }
    Assert-NoWarnings $log
    Invoke-Native ctest @('--test-dir', $dir, '--output-on-failure')
}

function Invoke-Plugins {
    Remove-Item -Recurse -Force -LiteralPath (Join-Path $Root 'dist') -ErrorAction SilentlyContinue
    $log = Join-Path $Root 'build\ci-plugins.log'
    # a child PowerShell, so everything build.ps1 shows (Out-Host included) reaches the log
    $pwsh = (Get-Process -Id $PID).Path
    Invoke-Logged $log { & $pwsh -NoProfile -ExecutionPolicy Bypass -File (Join-Path $Root 'build.ps1') -Sdk -OmcDir $OmcDir -OmcLibDir $OmcLib }
    Assert-NoWarnings $log
    Get-ChildItem (Join-Path $Root 'dist') | Format-Table Name, Length | Out-Host
}

# dumpbin's view of a DLL: its (delay-loaded) dependencies and its exports
function Get-Dependents([string]$Dll) {
    $out = & dumpbin /nologo /dependents $Dll
    $deps = @(); $delay = @(); $section = ''
    foreach ($l in $out) {
        if ($l -match 'has the following dependencies') { $section = 'deps'; continue }
        if ($l -match 'has the following delay load dependencies') { $section = 'delay'; continue }
        if ($l -match '^\s+Summary') { break }
        if ($l -match '^\s+(\S+\.(dll|DLL))\s*$') { if ($section -eq 'delay') { $delay += $Matches[1] } else { $deps += $Matches[1] } }
    }
    return @{ Deps = $deps; Delay = $delay }
}

function Get-Exports([string]$Dll) {
    $names = @()
    foreach ($l in (& dumpbin /nologo /exports $Dll)) {
        if ($l -match '^\s+\d+\s+[0-9A-F]+\s+[0-9A-F]{8}\s+(\S+)') { $names += $Matches[1] }
    }
    return ($names | Sort-Object)
}

function Test-Plugin([string]$Dll, [string]$HostName) {
    $d = Get-Dependents $Dll
    Write-Host "  imports $($d.Deps -join ' ')"
    Write-Host "  delay-loads $($d.Delay -join ' ')"
    $hostDlls = if ($HostName -eq 'binja') { 'binaryninjacore.dll', 'binaryninjaui.dll' } else { @('ida.dll') }
    foreach ($h in $hostDlls) { if ($d.Deps -notcontains $h) { Stop-Ci "$Dll does not import $h" } }
    if ($d.Delay -notcontains 'libomc.dll') { Stop-Ci "$Dll does not delay-load libomc.dll (it loads it from its own folder)" }
    $odd = @($d.Deps | Where-Object { $_ -notmatch '^(binaryninjacore|binaryninjaui|ida|Qt6Core|Qt6Gui|Qt6Widgets|KERNEL32|USER32|GDI32|ADVAPI32|SHELL32|ole32|OLEAUT32|WS2_32|bcrypt|CRYPT32|SHLWAPI|dbghelp|VERSION|MSVCP140(_[0-9a-z]+)?|VCRUNTIME140(_[0-9a-z]+)?|api-ms-win-[a-z0-9-]+)\.dll$' })
    if ($odd.Count) { Stop-Ci "$Dll imports $($odd -join ', '): only the host, Qt, Windows and the C++ runtime may be imported" }
    $exports = (Get-Exports $Dll) -join ' '
    Write-Host "  exports $exports"
    $want = if ($HostName -eq 'binja') { 'CorePluginABIVersion CorePluginInit UIPluginABIVersion UIPluginInit' } else { 'PLUGIN' }
    if ($exports -ne $want) { Stop-Ci "$Dll exports '$exports', not the host's entry points ($want)" }
}

function Invoke-Smoke {
    $n = 0
    foreach ($zip in Get-ChildItem (Join-Path $Root "dist\rikugan-$Version-*-windows-x86_64.zip")) {
        $want = ((Get-Content -Raw "$($zip.FullName).sha256").Trim() -split '\s+')[0]
        $got = (Get-FileHash -Algorithm SHA256 $zip.FullName).Hash.ToLowerInvariant()
        if ($want -ne $got) { Stop-Ci "$($zip.Name) does not match its .sha256" }
        $tmp = Join-Path ([IO.Path]::GetTempPath()) ('rk-smoke-' + [Guid]::NewGuid().ToString('N').Substring(0, 8))
        [IO.Compression.ZipFile]::ExtractToDirectory($zip.FullName, $tmp)
        $files = Get-ChildItem -Recurse -File $tmp | ForEach-Object { $_.FullName.Substring($tmp.Length + 1).Replace('\', '/') } | Sort-Object
        Write-Host "ci: $($zip.Name): $($files -join ' ')"
        if ($zip.Name -match '-binja-') {
            if (($files -join ' ') -ne 'rikugan.dll rikugan/libomc.dll') { Stop-Ci "$($zip.Name) holds $($files -join ', ')" }
            Test-Plugin (Join-Path $tmp 'rikugan.dll') 'binja'
        } else {
            if (($files -join ' ') -ne 'rikugan/ida-plugin.json rikugan/libomc.dll rikugan/rikugan.dll') { Stop-Ci "$($zip.Name) holds $($files -join ', ')" }
            Test-Plugin (Join-Path $tmp 'rikugan\rikugan.dll') 'ida'
        }
        $omc = (Get-Dependents (Get-ChildItem -Recurse $tmp -Filter libomc.dll | Select-Object -First 1).FullName).Deps
        $bad = @($omc | Where-Object { $_ -match '^(lib|zlib|cyg|msys-)' })
        if ($bad.Count) { Stop-Ci "libomc.dll imports $($bad -join ', '): it must need only Windows" }
        Remove-Item -Recurse -Force $tmp
        $n++
    }
    if ($n -ne 2) { Stop-Ci "expected the two plugins in dist\, found $n" }
    Write-Host 'ci: smoke ok'
}

Import-VsDevEnv
switch ($Step) {
    'test' { Invoke-Test }
    'plugins' { Invoke-Plugins }
    'smoke' { Invoke-Smoke }
}
