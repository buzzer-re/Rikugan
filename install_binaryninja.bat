@echo off
setlocal enabledelayedexpansion

:: Rikugan installer for Binary Ninja on Windows
:: Usage: install_binaryninja.bat [BN_USER_DIR]

set "SCRIPT_DIR=%~dp0"
if "%SCRIPT_DIR:~-1%"=="\" set "SCRIPT_DIR=%SCRIPT_DIR:~0,-1%"

if not exist "%SCRIPT_DIR%\rikugan_binaryninja.py" (
    echo [-] rikugan_binaryninja.py not found in %SCRIPT_DIR%
    exit /b 1
)
if not exist "%SCRIPT_DIR%\plugin.json" (
    echo [-] plugin.json not found in %SCRIPT_DIR%
    exit /b 1
)

set "BN_USER_DIR="
if not "%~1"=="" (
    if exist "%~1\" (
        set "BN_USER_DIR=%~1"
        echo [*] Using provided Binary Ninja directory: !BN_USER_DIR!
    ) else (
        echo [-] Provided Binary Ninja directory does not exist: %~1
        exit /b 1
    )
)

if not defined BN_USER_DIR (
    if exist "%APPDATA%\Binary Ninja\" (
        set "BN_USER_DIR=%APPDATA%\Binary Ninja"
        echo [*] Auto-detected Binary Ninja directory: !BN_USER_DIR!
    ) else (
        set "BN_USER_DIR=%USERPROFILE%\.binaryninja"
        echo [*] No Binary Ninja directory found, defaulting to !BN_USER_DIR!
    )
)

set "PLUGINS_DIR=%BN_USER_DIR%\plugins"
set "CONFIG_DIR=%BN_USER_DIR%\rikugan"
set "SKILLS_DIR=%CONFIG_DIR%\skills"

:: ── Remove old "iris" installation (rebrand cleanup) ───────────────
set "OLD_LINK=%PLUGINS_DIR%\iris"
if exist "%OLD_LINK%\" (
    fsutil reparsepoint query "%OLD_LINK%" >nul 2>&1
    if !errorlevel! equ 0 (
        echo [!] Removing old 'iris' plugin junction: %OLD_LINK%
        rmdir "%OLD_LINK%"
    ) else (
        echo [!] Removing old 'iris' plugin directory: %OLD_LINK%
        rmdir /s /q "%OLD_LINK%"
    )
    echo [+] Old 'iris' installation removed
)


:: ── Install Python dependencies ──────────────────────────────────
call :install_requirements
if !errorlevel! neq 0 (
    echo [-] Failed to install Python dependencies from requirements.txt
    exit /b 1
)

if not exist "%PLUGINS_DIR%\"  mkdir "%PLUGINS_DIR%"
if not exist "%SKILLS_DIR%\" mkdir "%SKILLS_DIR%"

set "BUILTINS_SRC=%SCRIPT_DIR%\rikugan\skills\builtins"
if exist "%BUILTINS_SRC%\" (
    echo [*] Installing built-in skills into %SKILLS_DIR%...
    for /d %%S in ("%BUILTINS_SRC%\*") do (
        set "SLUG=%%~nxS"
        if exist "%SKILLS_DIR%\!SLUG!\" (
            echo [+] /!SLUG! already exists, skipping ^(user copy preserved^)
        ) else (
            xcopy "%%S" "%SKILLS_DIR%\!SLUG!\" /E /I /Y /Q >nul
            echo [+] /!SLUG!
        )
    )
)

set "PLUGIN_LINK=%PLUGINS_DIR%\rikugan"
if exist "%PLUGIN_LINK%\" (
    fsutil reparsepoint query "%PLUGIN_LINK%" >nul 2>&1
    if !errorlevel! equ 0 (
        rmdir "%PLUGIN_LINK%"
    ) else (
        if exist "%PLUGINS_DIR%\rikugan.bak\" rmdir /s /q "%PLUGINS_DIR%\rikugan.bak"
        ren "%PLUGIN_LINK%" "rikugan.bak"
    )
)

mklink /J "%PLUGIN_LINK%" "%SCRIPT_DIR%" >nul 2>&1
if !errorlevel! neq 0 (
    echo [!] Junction failed, falling back to copy...
    xcopy "%SCRIPT_DIR%" "%PLUGIN_LINK%\" /E /I /Y /Q >nul
    if !errorlevel! neq 0 (
        echo [-] Failed to install plugin files
        exit /b 1
    )
)

echo.
echo [+] Rikugan Binary Ninja plugin installed successfully!
echo [*] Plugin: %PLUGIN_LINK%
echo [*] Config: %CONFIG_DIR%\
echo [*] Skills: %SKILLS_DIR%\
echo.
echo [*] Restart Binary Ninja and open Tools ^> Rikugan ^> Open Panel.

endlocal
exit /b 0

:: ── Subroutine: install_requirements ─────────────────────────────
:install_requirements
set "REQ=%SCRIPT_DIR%\requirements.txt"
if not exist "%REQ%" (
    echo [-] requirements.txt not found in %SCRIPT_DIR%
    exit /b 1
)

:: 1. Explicit override via BN_PYTHON env var
if defined BN_PYTHON (
    echo [*] Using BN_PYTHON override: %BN_PYTHON%
    "%BN_PYTHON%" -m pip install -r "%REQ%"
    if !errorlevel! equ 0 (
        echo [+] Dependencies installed with BN_PYTHON override
        exit /b 0
    )
    echo [!] BN_PYTHON override failed, trying other methods...
)

:: 2. Try Binary Ninja's bundled Python
:: Windows: BN ships a bundled Python with python.exe inside the install dir
set "BN_FOUND_PYTHON="
set "BN_FOUND_HOME="

if exist "%ProgramFiles%\Vector35\BinaryNinja\bundled-python3\python.exe" (
    set "BN_FOUND_PYTHON=%ProgramFiles%\Vector35\BinaryNinja\bundled-python3\python.exe"
    set "BN_FOUND_HOME=%ProgramFiles%\Vector35\BinaryNinja\bundled-python3"
)
if not defined BN_FOUND_PYTHON if exist "%LOCALAPPDATA%\Vector35\BinaryNinja\bundled-python3\python.exe" (
    set "BN_FOUND_PYTHON=%LOCALAPPDATA%\Vector35\BinaryNinja\bundled-python3\python.exe"
    set "BN_FOUND_HOME=%LOCALAPPDATA%\Vector35\BinaryNinja\bundled-python3"
)
if not defined BN_FOUND_PYTHON if exist "%ProgramFiles(x86)%\Vector35\BinaryNinja\bundled-python3\python.exe" (
    set "BN_FOUND_PYTHON=%ProgramFiles(x86)%\Vector35\BinaryNinja\bundled-python3\python.exe"
    set "BN_FOUND_HOME=%ProgramFiles(x86)%\Vector35\BinaryNinja\bundled-python3"
)

if defined BN_FOUND_PYTHON (
    echo [*] Found Binary Ninja Python: !BN_FOUND_PYTHON!
    set "PYTHONHOME=!BN_FOUND_HOME!"
    "!BN_FOUND_PYTHON!" -m pip install -r "%REQ%"
    if !errorlevel! equ 0 (
        set "PYTHONHOME="
        echo [+] Dependencies installed into Binary Ninja's Python
        exit /b 0
    )
    set "PYTHONHOME="
    echo [!] BN Python pip install failed, trying system Python...
)

:: 3. Fallback: system Python
where python3 >nul 2>&1
if !errorlevel! equ 0 (
    echo [*] Installing Python dependencies with: python3 -m pip
    python3 -m pip install -r "%REQ%"
    if !errorlevel! equ 0 (
        echo [+] Dependencies installed successfully
        exit /b 0
    )
    python3 -m pip install --user -r "%REQ%"
    if !errorlevel! equ 0 (
        echo [+] Dependencies installed successfully --user
        exit /b 0
    )
)

where python >nul 2>&1
if !errorlevel! equ 0 (
    echo [*] Installing Python dependencies with: python -m pip
    python -m pip install -r "%REQ%"
    if !errorlevel! equ 0 (
        echo [+] Dependencies installed successfully
        exit /b 0
    )
    python -m pip install --user -r "%REQ%"
    if !errorlevel! equ 0 (
        echo [+] Dependencies installed successfully --user
        exit /b 0
    )
)

where pip >nul 2>&1
if !errorlevel! equ 0 (
    echo [*] Installing Python dependencies with: pip
    pip install -r "%REQ%"
    if !errorlevel! equ 0 (
        echo [+] Dependencies installed successfully
        exit /b 0
    )
)

echo [-] Could not find a working Python/pip to install dependencies
exit /b 1
