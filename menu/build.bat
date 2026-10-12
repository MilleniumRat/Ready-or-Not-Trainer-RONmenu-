@echo off
setlocal
rem vcvars64 can report a failed vswhere lookup and still set up the compiler, so check for cl itself
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
where cl >nul 2>&1 || (echo cl.exe not found: check the Visual Studio path in build.bat & exit /b 1)
cd /d "%~dp0"
if not exist build mkdir build
rem A DLL that is loaded in the game cannot be overwritten, but it can be renamed:
rem move it aside so the new one can be written (unload the old one in game with END).
if exist build\RonMenu.old.dll del /q build\RonMenu.old.dll >nul 2>&1
if exist build\RonMenu.dll move /y build\RonMenu.dll build\RonMenu.old.dll >nul 2>&1

set IMGUI=third_party\imgui
set SRC=src\dllmain.cpp src\menu.cpp src\overlay.cpp src\esp.cpp src\stretch.cpp ^
 %IMGUI%\imgui.cpp %IMGUI%\imgui_draw.cpp %IMGUI%\imgui_tables.cpp %IMGUI%\imgui_widgets.cpp ^
 %IMGUI%\backends\imgui_impl_dx11.cpp %IMGUI%\backends\imgui_impl_dx12.cpp %IMGUI%\backends\imgui_impl_win32.cpp

cl /nologo /LD /MT /O2 /EHsc /std:c++17 /DNDEBUG /DUNICODE /D_UNICODE /W3 ^
   /I%IMGUI% /Isrc /Fobuild\ /Fe:build\RonMenu.dll %SRC% ^
   /link /NOLOGO user32.lib gdi32.lib dwmapi.lib imm32.lib d3d11.lib d3d12.lib dxgi.lib || exit /b 1

cl /nologo /MT /O2 /EHsc /DUNICODE /D_UNICODE /Fobuild\ /Fe:build\injector.exe injector\injector.cpp ^
   /link /NOLOGO user32.lib || exit /b 1

echo.
echo Built build\RonMenu.dll and build\injector.exe
