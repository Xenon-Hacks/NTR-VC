@echo off
rem Builds ntrviewer.exe (LITE, as the releases) with MSYS2 MINGW64 at C:\msys64.
rem Usage: build-local.bat [extra make arguments, e.g. clean or DEBUG=1]
rem See D:\Claude\BUILDING-NTR-VC.md for the one-time MSYS2 setup.
setlocal
set "BASH=C:\msys64\usr\bin\bash.exe"
if not exist "%BASH%" (
  echo MSYS2 not found at C:\msys64. See D:\Claude\BUILDING-NTR-VC.md.
  exit /b 1
)
set "MSYSTEM=MINGW64"
set "HERE=%~dp0"
if not exist "%HERE%lib" mkdir "%HERE%lib"
rem OS=Windows_NT must be passed explicitly: the MSYS2 login shell doesn't keep it, and without
rem it the Makefile takes the Linux path (no .exe, no Windows libraries, hundreds of link errors).
"%BASH%" -lc "cd \"$(cygpath -u '%HERE%')\" && make -j%NUMBER_OF_PROCESSORS% LITE=1 OS=Windows_NT %*"
if errorlevel 1 (
  echo Build FAILED.
  exit /b 1
)
echo Built "%HERE%ntrviewer.exe"
