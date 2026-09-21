@echo off
rem Installs TransperacyBar to %LOCALAPPDATA%\Programs\TransperacyBar and makes it start when you log in.
rem Run build.bat first.
setlocal
set ROOT=%~dp0
set DEST=%LOCALAPPDATA%\Programs\TransperacyBar

if not exist "%ROOT%bin\TransperacyBar.exe" (
    echo bin\TransperacyBar.exe not found. Run build.bat first.
    exit /b 1
)

taskkill /im TransperacyBar.exe /f >nul 2>nul
if not exist "%DEST%" mkdir "%DEST%"

copy /y "%ROOT%bin\TransperacyBar.exe" "%DEST%\" >nul || (echo Could not copy TransperacyBar.exe & exit /b 1)
if exist "%ROOT%bin\ExplorerHook.dll" (
    copy /y "%ROOT%bin\ExplorerHook.dll" "%DEST%\" >nul 2>nul || (
        echo ExplorerHook.dll is in use by explorer.exe, so the installed copy was not updated.
        echo Restart explorer, then run install.bat again.
    )
)

rem Same entry the app's "Start with Windows" menu item uses; it shows up under Settings ^> Apps ^> Startup.
reg add "HKCU\Software\Microsoft\Windows\CurrentVersion\Run" /v TransperacyBar /t REG_SZ /d "\"%DEST%\TransperacyBar.exe\"" /f >nul

start "" "%DEST%\TransperacyBar.exe"
echo Installed to %DEST% and set to start when you log in.
