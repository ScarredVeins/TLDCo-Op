@echo off
rem Builds build\winhttp.dll (Release, x64). Usage: build.cmd [Debug|Release]
setlocal
set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=Release

for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSDIR=%%i
if "%VSDIR%"=="" (echo Visual Studio C++ build tools not found & exit /b 1)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

cd /d "%~dp0"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=%CONFIG% || exit /b 1
cmake --build build || exit /b 1
echo Built %~dp0build\winhttp.dll
