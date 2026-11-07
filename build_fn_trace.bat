@echo off
REM ===========================================================================
REM  ConquestTrace.dll + trace_launch.exe — the retail function tracer,
REM  standalone so it never has to fight the RTX Remix wrapper for d3d9.dll.
REM
REM  x86, to match the 2009 game image. Also rebuilds retail_symbols.bin from
REM  the corpus + the port tree, because a stale symbol table is worse than no
REM  symbol table: it names the wrong function at the right address.
REM ===========================================================================
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul
if not exist out mkdir out

echo [1/3] baking retail_symbols.bin
python tools\build_retail_symbols.py
if errorlevel 1 goto :fail

echo [2/3] ConquestTrace.dll
cl /nologo /LD /O2 /EHsc /MT /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
   /D_CRT_SECURE_NO_WARNINGS /Isrc ^
   src\retail_fn_trace.cpp src\trace_standalone.cpp ^
   /Fo:out\ /Fe:out\ConquestTrace.dll /link kernel32.lib user32.lib
if errorlevel 1 goto :fail

echo [3/4] trace_launch.exe
cl /nologo /O2 /EHsc /MT /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS ^
   tools\trace_launch.cpp /Fo:out\ /Fe:out\trace_launch.exe ^
   /link kernel32.lib
if errorlevel 1 goto :fail

REM  The falsifier. /Od because the self-test calls functions it generated at
REM  runtime through pointers the optimiser is entitled to fold away.
echo [4/4] trace_selftest.exe
if not exist out\st mkdir out\st
cl /nologo /Od /EHsc /MT /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS /Isrc ^
   tools\trace_selftest.cpp src\retail_fn_trace.cpp ^
   /Fo:out\st\ /Fe:out\trace_selftest.exe /link kernel32.lib user32.lib
if errorlevel 1 goto :fail

echo.
echo BUILD_OK
echo   out\ConquestTrace.dll     inject this
echo   out\trace_launch.exe      ...or launch through this to catch boot
echo   out\retail_symbols.bin    must sit NEXT TO the DLL
echo   out\retail_symbols.txt    the same table, greppable
echo   out\trace_selftest.exe    proves the mechanism without the game

if /I not "%1"=="deploy" (
  echo.
  echo Run `build_fn_trace.bat deploy` to copy into RE\ and the game folder.
  exit /b 0
)

REM ---------------------------------------------------------------------------
REM  DEPLOY — additive only. d3d9.dll is NEVER touched: in the game folder that
REM  name belongs to the RTX Remix FFP wrapper, and replacing it kills the whole
REM  RTX path. The tracer needs no D3D, so it lives beside it instead.
REM ---------------------------------------------------------------------------
set RE=..\..\..\RE
set GAME=..\..\..\..\..\..\..\..\..\..
call :push "%RE%"
call :push "%GAME%"
echo.
echo DEPLOY_OK
exit /b 0

:push
if not exist "%~1\." exit /b 0
copy /Y out\ConquestTrace.dll  "%~1\" >nul
copy /Y out\retail_symbols.bin "%~1\" >nul
copy /Y out\retail_symbols.txt "%~1\" >nul
copy /Y out\trace_launch.exe   "%~1\" >nul
if not exist "%~1\trace_presets\." mkdir "%~1\trace_presets"
copy /Y presets\*.ini "%~1\trace_presets\" >nul
copy /Y presets\README.txt "%~1\trace_presets\" >nul
REM The live config defaults to EVERYTHING WE HAVE DECODED — 1,680 ported
REM functions, every call counted. Swap in any file from trace_presets\ to
REM change the question. An existing ini is never overwritten.
if not exist "%~1\retail_trace.ini" (
  copy /Y presets\01_safe.ini "%~1\retail_trace.ini" >nul
  echo   wrote %~1\retail_trace.ini  ^(preset: 01_safe^)
)
echo   deployed to %~1
exit /b 0

:fail
echo BUILD_FAILED errorlevel=%errorlevel%
exit /b 1
