@echo off
rem Builds SpenBridge.exe (single native exe, static CRT, no runtime needed).
setlocal
cd /d "%~dp0"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
if not defined VS (
  echo Visual Studio C++ build tools not found.
  exit /b 1
)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not exist build mkdir build
cl /nologo /std:c++17 /O2 /MT /EHsc /W3 /DUNICODE /D_UNICODE spenbridge.cpp /Fo:build\ /Fe:build\SpenBridge.exe /link /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF || exit /b 1
echo Built %CD%\build\SpenBridge.exe
