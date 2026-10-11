@echo off
setlocal
if /i "%VSCMD_ARG_TGT_ARCH%"=="x86" goto compile
set "ARKHAM_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%ARKHAM_VSWHERE%" (
  echo MSVC x86 tools were not found. Install Visual Studio Build Tools with C++ tools.
  exit /b 1
)
for /f "usebackq tokens=*" %%I in (`"%ARKHAM_VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "ARKHAM_VS=%%I"
if not defined ARKHAM_VS exit /b 1
call "%ARKHAM_VS%\VC\Auxiliary\Build\vcvars32.bat"
if errorlevel 1 exit /b 1
:compile
cd /d "%~dp0"
if not exist build mkdir build
cl /nologo /W4 /O2 /MT /EHsc /LD src\SubtitleScale.cpp /Fobuild\SubtitleScale.obj /Febuild\ArkhamSubtitleScale.asi /link /MACHINE:X86 advapi32.lib
if errorlevel 1 exit /b 1
cl /nologo /W4 /O2 /MT /EHsc /LD src\Loader.cpp /Fobuild\Loader.obj /Febuild\dinput8.dll /link /MACHINE:X86 /DEF:src\Loader.def
if errorlevel 1 exit /b 1
cl /nologo /W4 /O2 /MT /EHsc src\Tests.cpp /Fobuild\Tests.obj /Febuild\Tests.exe /link /MACHINE:X86 advapi32.lib
if errorlevel 1 exit /b 1
build\Tests.exe %*
exit /b %errorlevel%
