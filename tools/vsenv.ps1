# Dot-sourced by build.ps1 and ci-local.ps1: Import-VsDevEnv loads the x64 MSVC environment of the newest Visual
# Studio with the C++ tools (cl, link, lib, and its bundled CMake and Ninja) into the current PowerShell process.
function Import-VsDevEnv {
    if ((Get-Command cl.exe -ErrorAction SilentlyContinue) -and (Get-Command ninja.exe -ErrorAction SilentlyContinue)) { return }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) { throw 'Visual Studio was not found (install it with "Desktop development with C++").' }
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw 'No Visual Studio with the x64 C++ tools ("Desktop development with C++") was found.' }
    $vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
    $lines = & cmd.exe /d /c "`"$vcvars`" >nul && set"
    if ($LASTEXITCODE -ne 0) { throw "$vcvars failed" }
    foreach ($line in $lines) {
        $eq = $line.IndexOf('=')
        if ($eq -gt 0) { [Environment]::SetEnvironmentVariable($line.Substring(0, $eq), $line.Substring($eq + 1), 'Process') }
    }
    foreach ($tool in 'cl.exe', 'cmake.exe', 'ninja.exe') {
        if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
            throw "$tool is missing from $vs (add the workload's ""C++ CMake tools for Windows"")"
        }
    }
}

# Runs a native command and stops on a non-zero exit status.
function Invoke-Native([string]$Exe, [string[]]$Arguments) {
    & $Exe @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Exe $($Arguments -join ' ') failed (exit $LASTEXITCODE)" }
}
