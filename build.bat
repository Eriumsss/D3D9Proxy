@echo off
setlocal
set "ROOT=%~dp0"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo [ERROR] vswhere.exe not found. Install Visual Studio with MSVC x86 tools.
    exit /b 2
)
for /f "usebackq delims=" %%V in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%V"
if not defined VSROOT (
    echo [ERROR] MSVC x86 toolchain not installed.
    exit /b 3
)
call "%VSROOT%\VC\Auxiliary\Build\vcvarsall.bat" x86
if errorlevel 1 exit /b 1

if not exist "%ROOT%out" mkdir "%ROOT%out"
pushd "%ROOT%src"
if errorlevel 1 exit /b 1
del /q *.obj 2>nul
cl /nologo /c /EHsc /W0 /MD /O2 /D WIN32 /D NDEBUG /D WIN32_LEAN_AND_MEAN /D NOMINMAX /D _WINDLL ^
 d3d9device.cpp d3d9proxy.cpp overlay.cpp memorypatch.cpp filehook.cpp vespucci_bridge.cpp stronghold_log.cpp crowd_event_handler.cpp retail_fn_trace.cpp retail_heap_extender.cpp static_pool_census.cpp model_registry_2048.cpp texture_registry_4096.cpp effect_registry_1024.cpp gfx_registry_512.cpp auxiliary_registry_8192.cpp geometry_budget_telemetry.cpp retail_texture_vault.cpp
if errorlevel 1 goto :failed
link /DLL /OUT:"%ROOT%out\d3d9.dll" /DEF:d3d9proxy.def /NOLOGO ^
 d3d9device.obj d3d9proxy.obj overlay.obj memorypatch.obj filehook.obj vespucci_bridge.obj stronghold_log.obj crowd_event_handler.obj retail_fn_trace.obj retail_heap_extender.obj static_pool_census.obj model_registry_2048.obj texture_registry_4096.obj effect_registry_1024.obj gfx_registry_512.obj auxiliary_registry_8192.obj geometry_budget_telemetry.obj retail_texture_vault.obj ^
 d3d9.lib dxguid.lib uuid.lib user32.lib kernel32.lib
if errorlevel 1 goto :failed
dumpbin /headers "%ROOT%out\d3d9.dll" | findstr /i "machine x86"
if errorlevel 1 goto :failed
popd
echo [OK] out\d3d9.dll built for x86.
exit /b 0
:failed
popd
echo [ERROR] Standalone D3D9Proxy x86 build failed.
exit /b 1
