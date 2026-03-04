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


if not exist "%PLUGINS_DIR%\" mkdir "%PLUGINS_DIR%"
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

