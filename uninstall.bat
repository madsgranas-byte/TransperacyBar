@echo off
rem Removes TransperacyBar from startup and deletes the installed files.
setlocal
set DEST=%LOCALAPPDATA%\Programs\TransperacyBar

taskkill /im TransperacyBar.exe /f >nul 2>nul
reg delete "HKCU\Software\Microsoft\Windows\CurrentVersion\Run" /v TransperacyBar /f >nul 2>nul

del /q "%DEST%\TransperacyBar.exe" >nul 2>nul
del /q "%DEST%\ExplorerHook.dll" >nul 2>nul
if exist "%DEST%\ExplorerHook.dll" (
    echo ExplorerHook.dll is still loaded by explorer.exe. Restart explorer or sign out to get the normal
    echo taskbar back, then run uninstall.bat again to delete it.
    exit /b 1
)
rmdir "%DEST%" >nul 2>nul
echo TransperacyBar was uninstalled.
