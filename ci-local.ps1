# Local CI on Windows (twin of ci-local.sh): builds the test tree and runs the native test suite, then builds the host
# plugins with build.ps1 (IDA always, Binary Ninja when installed).
#
#   .\ci-local.ps1                         # needs omc's source at ..\ohmyc (or $env:OMC_DIR) for the mock model
#   $env:OMC_LIB_DIR = '...'; .\ci-local.ps1   # a prebuilt libomc instead of building it from the source
#   .\ci-local.ps1 -Python C:\Python312\python.exe   # when no python.exe is on PATH
param([string]$OmcDir = $env:OMC_DIR, [string]$Python = $env:PYTHON)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$Root = $PSScriptRoot
. (Join-Path $Root 'tools\vsenv.ps1')

if (-not $OmcDir) { $OmcDir = Join-Path (Split-Path $Root -Parent) 'ohmyc' }
if (-not (Test-Path (Join-Path $OmcDir 'include\omc.h'))) { throw "omc not found at $OmcDir (set OMC_DIR): the tests run its mock model" }
if (-not $Python) {
    $Python = Get-Command python.exe, python3.exe -ErrorAction SilentlyContinue |
        Where-Object { $_.Source -notmatch 'WindowsApps' } | Select-Object -First 1 -ExpandProperty Source
}
if (-not $Python) { throw 'Python 3 is not on PATH (the tests run omc''s mock model with it); pass -Python' }

Import-VsDevEnv
Push-Location $Root
try { Invoke-Native git @('submodule', 'update', '--init', '--recursive', 'third_party') } finally { Pop-Location }

$qt = Join-Path $HOME 'Qt\6.11.1\msvc2022_64'
if (-not (Test-Path (Join-Path $qt 'lib\cmake\Qt6\Qt6Config.cmake'))) { & (Join-Path $Root 'tools\fetch_qt.ps1') 6.11.1 | Out-Null }

Write-Host "`n== tests (Qt 6.11.1)" -ForegroundColor Cyan
$cfg = @('-S', $Root, '-B', (Join-Path $Root 'build\none'), '-G', 'Ninja', '-DRIKUGAN_HOST=none', "-DOMC_DIR=$OmcDir",
         "-DCMAKE_PREFIX_PATH=$qt", "-DPython3_EXECUTABLE=$Python", "-DOMC_LIB_DIR=$env:OMC_LIB_DIR")
Invoke-Native cmake $cfg
Invoke-Native cmake @('--build', (Join-Path $Root 'build\none'))
Invoke-Native ctest @('--test-dir', (Join-Path $Root 'build\none'), '--output-on-failure')

Write-Host "`n== host plugins" -ForegroundColor Cyan
& (Join-Path $Root 'build.ps1') -OmcDir $OmcDir
Write-Host "`n== OK" -ForegroundColor Green
