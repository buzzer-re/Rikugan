@echo off
setlocal enabledelayedexpansion

:: Rikugan installer for Windows
:: Usage: install.bat [IDA_USER_DIR]
::   IDA_USER_DIR  Optional path to IDA user directory (default: auto-detect)

set "SCRIPT_DIR=%~dp0"
:: Remove trailing backslash
if "%SCRIPT_DIR:~-1%"=="\" set "SCRIPT_DIR=%SCRIPT_DIR:~0,-1%"

:: ── Sanity checks ────────────────────────────────────────────────────

if not exist "%SCRIPT_DIR%\rikugan_plugin.py" (
    echo [-] rikugan_plugin.py not found in %SCRIPT_DIR% — run this from the repo root
    exit /b 1
)

if not exist "%SCRIPT_DIR%\rikugan\" (
    echo [-] rikugan\ package not found in %SCRIPT_DIR% — run this from the repo root
    exit /b 1
)

:: ── Locate IDA user directory ────────────────────────────────────────

set "IDA_USER_DIR="

if not "%~1"=="" (
    if exist "%~1\" (
        set "IDA_USER_DIR=%~1"
        echo [*] Using provided IDA directory: !IDA_USER_DIR!
    ) else (
        echo [-] Provided IDA directory does not exist: %~1
        exit /b 1
    )
)

if not defined IDA_USER_DIR (
    :: Try common Windows IDA locations
    if exist "%APPDATA%\Hex-Rays\IDA Pro\" (
        set "IDA_USER_DIR=%APPDATA%\Hex-Rays\IDA Pro"
        echo [*] Auto-detected IDA directory: !IDA_USER_DIR!
    ) else if exist "%USERPROFILE%\.idapro\" (
        set "IDA_USER_DIR=%USERPROFILE%\.idapro"
        echo [*] Auto-detected IDA directory: !IDA_USER_DIR!
    ) else if defined IDAUSR (
        set "IDA_USER_DIR=%IDAUSR%"
        echo [*] Auto-detected IDA directory via IDAUSR: !IDA_USER_DIR!
    ) else (
        set "IDA_USER_DIR=%APPDATA%\Hex-Rays\IDA Pro"
        echo [!] No IDA directory found, defaulting to !IDA_USER_DIR!
    )
)

set "PLUGINS_DIR=%IDA_USER_DIR%\plugins"
set "CONFIG_DIR=%IDA_USER_DIR%\rikugan"

:: ── Remove old "iris" installation (rebrand cleanup) ───────────────
if exist "%PLUGINS_DIR%\iris_plugin.py" (
    echo [!] Removing old iris_plugin.py
    del "%PLUGINS_DIR%\iris_plugin.py"
    echo [+] Old iris_plugin.py removed
)
set "OLD_IRIS=%PLUGINS_DIR%\iris"
if exist "%OLD_IRIS%\" (
    fsutil reparsepoint query "%OLD_IRIS%" >nul 2>&1
    if !errorlevel! equ 0 (
        echo [!] Removing old 'iris' plugin junction: %OLD_IRIS%
        rmdir "%OLD_IRIS%"
    ) else (
        echo [!] Removing old 'iris' plugin directory: %OLD_IRIS%
        rmdir /s /q "%OLD_IRIS%"
    )
    echo [+] Old 'iris' installation removed
)

:: ── Find IDA installation directory ──────────────────────────────────

set "IDA_INSTALL_DIR="
for /f "tokens=2*" %%A in ('reg query "HKCU\Software\Hex-Rays\IDA" /v "Location" 2^>nul') do set "IDA_INSTALL_DIR=%%B"
if not defined IDA_INSTALL_DIR (
    for /f "tokens=2*" %%A in ('reg query "HKLM\SOFTWARE\Hex-Rays\IDA" /v "Location" 2^>nul') do set "IDA_INSTALL_DIR=%%B"
)

:: ── Find IDA's Python ─────────────────────────────────────────────────

set "IDA_PYTHON="
if defined IDA_INSTALL_DIR (
    echo [*] IDA install dir: !IDA_INSTALL_DIR!
    :: Bundled Python: <IDA>\python3.XX\python.exe  (IDA 7.5+)
    for /d %%D in ("!IDA_INSTALL_DIR!\python3*") do (
        if exist "%%D\python.exe" set "IDA_PYTHON=%%D\python.exe"
    )
    :: Older bundled layout: <IDA>\python\python.exe
    if not defined IDA_PYTHON (
        if exist "!IDA_INSTALL_DIR!\python\python.exe" (
            set "IDA_PYTHON=!IDA_INSTALL_DIR!\python\python.exe"
        )
    )
    :: idapyswitch: asks IDA which system Python it was switched to
    if not defined IDA_PYTHON (
        if exist "!IDA_INSTALL_DIR!\idapyswitch.exe" (
            for /f "usebackq tokens=* delims=" %%L in (`"!IDA_INSTALL_DIR!\idapyswitch.exe" --show-current 2^>nul`) do (
                :: Output is typically a bare path or "Path: C:\..."
                set "_line=%%L"
                set "_line=!_line:Path: =!"
                if exist "!_line!" set "IDA_PYTHON=!_line!"
            )
        )
    )
)

:: ── Install dependencies ─────────────────────────────────────────────

echo [*] Installing Python dependencies...

if defined IDA_PYTHON (
    echo [*] Using IDA's Python: !IDA_PYTHON!
    set "PIP_CMD=!IDA_PYTHON! -m pip"
    call :try_install_requirements
    if !errorlevel! equ 0 goto deps_ok
    echo [!] IDA Python pip failed, trying system Python fallbacks...
)

set "PIP_CMD=py -3 -m pip"
call :try_install_requirements
if !errorlevel! equ 0 goto deps_ok
set "PIP_CMD=python3 -m pip"
call :try_install_requirements
if !errorlevel! equ 0 goto deps_ok
set "PIP_CMD=python -m pip"
call :try_install_requirements
if !errorlevel! equ 0 goto deps_ok
set "PIP_CMD=pip3"
call :try_install_requirements
if !errorlevel! equ 0 goto deps_ok
set "PIP_CMD=pip"
call :try_install_requirements
if !errorlevel! equ 0 goto deps_ok

echo [-] Failed to install Python dependencies from requirements.txt
exit /b 1

:deps_ok

:: ── Create directories ───────────────────────────────────────────────

if not exist "%PLUGINS_DIR%\" mkdir "%PLUGINS_DIR%"
if not exist "%CONFIG_DIR%\"  mkdir "%CONFIG_DIR%"

:: ── Copy built-in skills ────────────────────────────────────────────

set "SKILLS_DIR=%CONFIG_DIR%\skills"
set "BUILTINS_SRC=%SCRIPT_DIR%\rikugan\skills\builtins"

if exist "%BUILTINS_SRC%\" (
    echo [*] Installing built-in skills into %SKILLS_DIR%...
    if not exist "%SKILLS_DIR%\" mkdir "%SKILLS_DIR%"
    for /d %%S in ("%BUILTINS_SRC%\*") do (
        set "SLUG=%%~nxS"
        if exist "%SKILLS_DIR%\!SLUG!\" (
            echo [+] /!SLUG! already exists, skipping ^(user copy preserved^)
        ) else (
            xcopy "%%S" "%SKILLS_DIR%\!SLUG!\" /E /I /Y /Q >nul
            echo [+] /!SLUG!
        )
    )
) else (
    echo [!] Built-in skills not found at %BUILTINS_SRC%, skipping
)

:: ── Install plugin (copy) ────────────────────────────────────────────

echo [*] Installing Rikugan into %PLUGINS_DIR%...

:: rikugan_plugin.py
if exist "%PLUGINS_DIR%\rikugan_plugin.py" (
    del "%PLUGINS_DIR%\rikugan_plugin.py"
)
copy "%SCRIPT_DIR%\rikugan_plugin.py" "%PLUGINS_DIR%\rikugan_plugin.py" >nul
if !errorlevel! equ 0 (
    echo [+] rikugan_plugin.py -^> %PLUGINS_DIR%\rikugan_plugin.py
) else (
    echo [-] Failed to copy rikugan_plugin.py
    exit /b 1
)

:: rikugan/ package — use directory junction (symlink-like, no admin required)
if exist "%PLUGINS_DIR%\rikugan\" (
    :: Check if it's a junction
    fsutil reparsepoint query "%PLUGINS_DIR%\rikugan" >nul 2>&1
    if !errorlevel! equ 0 (
        rmdir "%PLUGINS_DIR%\rikugan"
    ) else (
        :: Real directory — back it up
        echo [!] Backing up existing rikugan\ to rikugan.bak\
        if exist "%PLUGINS_DIR%\rikugan.bak\" rmdir /s /q "%PLUGINS_DIR%\rikugan.bak"
        ren "%PLUGINS_DIR%\rikugan" "rikugan.bak"
    )
)

mklink /J "%PLUGINS_DIR%\rikugan" "%SCRIPT_DIR%\rikugan" >nul 2>&1
if !errorlevel! equ 0 (
    echo [+] rikugan\ -^> %PLUGINS_DIR%\rikugan  (junction^)
) else (
    :: Junction failed (rare), fall back to xcopy
    echo [*] Junction failed, falling back to copy...
    xcopy "%SCRIPT_DIR%\rikugan" "%PLUGINS_DIR%\rikugan\" /E /I /Y /Q >nul
    if !errorlevel! equ 0 (
        echo [+] rikugan\ -^> %PLUGINS_DIR%\rikugan  (copied^)
    ) else (
        echo [-] Failed to copy rikugan\ package
        exit /b 1
    )
)

:: ── Done ─────────────────────────────────────────────────────────────

echo.
echo [+] Rikugan installed successfully!
echo [*] Plugin:  %PLUGINS_DIR%\rikugan_plugin.py
echo [*] Package: %PLUGINS_DIR%\rikugan
echo [*] Config:  %CONFIG_DIR%\
echo [*] Skills:  %SKILLS_DIR%\
echo.
echo [*] Open IDA and press Ctrl+Shift+I to start Rikugan.
echo [*] First run: click Settings to configure your LLM provider and API key.
echo [*] For Binary Ninja installation, run install_binaryninja.bat

endlocal
exit /b 0

:try_install_requirements
%PIP_CMD% --version >nul 2>&1
if errorlevel 1 exit /b 1
echo [*] Installing dependencies with %PIP_CMD%
%PIP_CMD% install -r "%SCRIPT_DIR%\requirements.txt"
if errorlevel 1 (
    echo [!] Dependency install failed with %PIP_CMD%
    exit /b 1
)
echo [+] Dependencies installed successfully
exit /b 0
