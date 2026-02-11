@echo off
setlocal enabledelayedexpansion

set MSYS2=C:\msys64
set BASH=%MSYS2%\usr\bin\bash.exe
set OUT=wooting-aim.exe

:: Get script directory (where build.bat lives)
set "PROJDIR=%~dp0"
:: Remove trailing backslash
if "%PROJDIR:~-1%"=="\" set "PROJDIR=%PROJDIR:~0,-1%"
:: Convert to POSIX path: C:\Users\... -> /c/Users/...
set "POSIX=%PROJDIR:\=/%"
set "POSIX=/!POSIX:~0,1!!POSIX:~2!"
:: Lowercase drive letter
for %%a in (a b c d e f g h i j k l m n o p q r s t u v w x y z) do set "POSIX=!POSIX:/%%a/=/%%a/!"

:: Check MSYS2 exists
if not exist "%BASH%" (
    echo [ERROR] MSYS2 not found at %MSYS2%
    echo Download from https://www.msys2.org/
    echo Then: pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-hidapi
    exit /b 1
)

:: Kill running instance if any
tasklist /fi "IMAGENAME eq %OUT%" /nh 2>nul | findstr /i "%OUT%" >nul
if %errorlevel%==0 (
    echo [BUILD] Stopping running %OUT%...
    taskkill /f /im %OUT% >nul 2>&1
    timeout /t 1 /nobreak >nul
)

echo [BUILD] Compiling wooting-aim v0.7...
echo [BUILD] Project: %PROJDIR%
"%BASH%" -lc "cd '%POSIX%' && gcc -O2 -Wall -g -I./include -I/mingw64/include -o wooting-aim.exe src/main.c src/hid_writer.c -L./lib -L/mingw64/lib -lwooting_analog_sdk -lhidapi -lsetupapi -lws2_32 -ladvapi32"

if %errorlevel%==0 (
    echo [BUILD] OK: %OUT%
) else (
    echo [BUILD] FAILED
    exit /b 1
)

echo [BUILD] Compiling hid-enum...
"%BASH%" -lc "cd '%POSIX%' && gcc -O2 -Wall -I./include -I/mingw64/include -o hid-enum.exe src/hid_enum.c -L./lib -L/mingw64/lib -lhidapi -lsetupapi"

if %errorlevel%==0 (
    echo [BUILD] OK: hid-enum.exe
) else (
    echo [BUILD] hid-enum failed, non-critical
)

echo.
echo Done. Run with: %OUT% --adaptive
endlocal
