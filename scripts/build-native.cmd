@echo off
setlocal
if not defined VSCMD_VER (
  for /f "usebackq tokens=*" %%I in (`"C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "SQUEEZE_VS=%%I"
)
if not defined VSCMD_VER call "%SQUEEZE_VS%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
pushd "%~dp0.."
if not exist build mkdir build
cl /nologo /std:c++20 /utf-8 /O1 /GL /Gy /MT /W4 /EHsc /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /Isrc /Igenerated /Fo:build\ /c src\app.cpp src\engine.cpp
if errorlevel 1 exit /b 1
rc /nologo /i assets /fo build\app.res assets\app.rc
if errorlevel 1 exit /b 1
link /nologo /LTCG /OPT:REF /OPT:ICF /SUBSYSTEM:WINDOWS /MANIFEST:NO /OUT:build\Squeeze.exe build\app.obj build\engine.obj build\app.res d2d1.lib dwrite.lib windowscodecs.lib ole32.lib shell32.lib dwmapi.lib dxgi.lib bcrypt.lib cabinet.lib comctl32.lib user32.lib gdi32.lib uuid.lib
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /utf-8 /O1 /GL /Gy /MT /W4 /EHsc /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /Isrc /Igenerated /Fo:build\core_tests.obj /c tests\core_tests.cpp
if errorlevel 1 exit /b 1
link /nologo /LTCG /OPT:REF /OPT:ICF /SUBSYSTEM:CONSOLE /MANIFEST:NO /OUT:build\SqueezeTests.exe build\core_tests.obj build\engine.obj build\app.res ole32.lib shell32.lib dxgi.lib bcrypt.lib cabinet.lib uuid.lib
if errorlevel 1 exit /b 1
if "%~1"=="ui-tests" (
  cl /nologo /std:c++20 /utf-8 /O1 /GL /Gy /MT /W4 /EHsc /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DSQUEEZE_UI_TESTING /Isrc /Igenerated /Fo:build\app_ui_tests.obj /c src\app.cpp
  if errorlevel 1 exit /b 1
  link /nologo /LTCG /OPT:REF /OPT:ICF /SUBSYSTEM:WINDOWS /MANIFEST:NO /OUT:build\SqueezeUiTests.exe build\app_ui_tests.obj build\engine.obj build\app.res d2d1.lib dwrite.lib windowscodecs.lib ole32.lib shell32.lib dwmapi.lib dxgi.lib bcrypt.lib cabinet.lib comctl32.lib user32.lib gdi32.lib uuid.lib
  if errorlevel 1 exit /b 1
)
popd
exit /b 0
