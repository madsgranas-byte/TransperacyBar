@echo off
rem Builds bin\TransperacyBar.exe with the C# compiler that ships with Windows (.NET Framework 4.x),
rem and bin\ExplorerHook.dll (needed on Windows 11 22H2+) with the Visual Studio C++ tools.
setlocal
set ROOT=%~dp0
if not exist "%ROOT%bin" mkdir "%ROOT%bin"

set CSC=%WINDIR%\Microsoft.NET\Framework64\v4.0.30319\csc.exe
if not exist "%CSC%" (
    echo Could not find csc.exe from .NET Framework 4.
    exit /b 1
)
"%CSC%" /nologo /target:winexe /optimize+ /platform:x64 ^
    /out:"%ROOT%bin\TransperacyBar.exe" ^
    /reference:System.Windows.Forms.dll /reference:System.Drawing.dll ^
    "%ROOT%src\TransperacyBar.cs"
if errorlevel 1 exit /b 1
echo Built bin\TransperacyBar.exe

set VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
if not exist "%VSWHERE%" goto nocpp
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSDIR=%%i
if not defined VSDIR goto nocpp

call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
if not exist "%ROOT%obj" mkdir "%ROOT%obj"
cl /nologo /std:c++17 /EHsc /O2 /MT /W3 /LD /Fo"%ROOT%obj\\" ^
    "%ROOT%hook\ExplorerHook.cpp" ^
    /Fe"%ROOT%obj\ExplorerHook.dll" ^
    /link /DEF:"%ROOT%hook\ExplorerHook.def" windowsapp.lib user32.lib ole32.lib
if errorlevel 1 exit /b 1
copy /y "%ROOT%obj\ExplorerHook.dll" "%ROOT%bin\ExplorerHook.dll" >nul 2>nul
if errorlevel 1 (
    echo bin\ExplorerHook.dll is in use by explorer.exe. Restart explorer, then run build.bat again.
    exit /b 1
)
echo Built bin\ExplorerHook.dll
exit /b 0

:nocpp
echo Visual Studio C++ tools not found; skipped bin\ExplorerHook.dll (only needed on Windows 11 22H2 and later).
exit /b 0
