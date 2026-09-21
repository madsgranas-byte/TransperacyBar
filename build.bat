@echo off
rem Builds TransperacyBar.exe with the C# compiler that ships with Windows (.NET Framework 4.x).
setlocal
set CSC=%WINDIR%\Microsoft.NET\Framework64\v4.0.30319\csc.exe
if not exist "%CSC%" set CSC=%WINDIR%\Microsoft.NET\Framework\v4.0.30319\csc.exe
if not exist "%CSC%" (
    echo Could not find csc.exe from .NET Framework 4.
    exit /b 1
)

if not exist "%~dp0bin" mkdir "%~dp0bin"
"%CSC%" /nologo /target:winexe /optimize+ /platform:anycpu ^
    /out:"%~dp0bin\TransperacyBar.exe" ^
    /reference:System.Windows.Forms.dll /reference:System.Drawing.dll ^
    "%~dp0src\TransperacyBar.cs"
if errorlevel 1 exit /b 1
echo Built bin\TransperacyBar.exe
