@echo off
rem Builds Forge PC (forge.dll) and the example plugin with Visual Studio 2022 (x64), and lays
rem out an install folder: dist\mods\0100770008DD8000\  -> copy "mods" into the game folder.
setlocal
cd /d "%~dp0"
where cl >nul 2>&1 && goto :env_ok
"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath > "%TEMP%\forge_vsdir.txt"
set /p VSDIR=<"%TEMP%\forge_vsdir.txt"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
:env_ok
if not defined VULKAN_SDK for /d %%d in ("C:\VulkanSDK\*") do if exist "%%d\Include\vulkan\vulkan.h" set "VULKAN_SDK=%%d"
if not exist "%VULKAN_SDK%\Include\vulkan\vulkan.h" (
  echo [error] Vulkan SDK headers not found ^(C:\VulkanSDK^). Only the headers are used.
  exit /b 1
)
set "OUT=dist\mods\0100770008DD8000"
set "OBJ=build\obj"
if not exist "%OBJ%" mkdir "%OBJ%"
if not exist "%OUT%\Forge\fonts" mkdir "%OUT%\Forge\fonts"
if not exist "%OUT%\HudFix\plugins" mkdir "%OUT%\HudFix\plugins"
if not exist "build\examples" mkdir "build\examples"
if not exist "%OBJ%\hudfix" mkdir "%OBJ%\hudfix"

set "CFLAGS=/nologo /O2 /MT /EHsc /std:c++20 /W3 /utf-8 /DVK_NO_PROTOTYPES /DIMGUI_IMPL_VULKAN_NO_PROTOTYPES /DNOMINMAX /Iinclude /Ithird_party\imgui /I"%VULKAN_SDK%\Include""
cl %CFLAGS% /c /Fo%OBJ%\ src\loader.cpp src\overlay.cpp third_party\imgui\imgui.cpp third_party\imgui\imgui_draw.cpp third_party\imgui\imgui_tables.cpp third_party\imgui\imgui_widgets.cpp third_party\imgui\imgui_demo.cpp third_party\imgui\backends\imgui_impl_vulkan.cpp || exit /b 1
link /nologo /DLL /OUT:"%OUT%\Forge\forge.dll" /IMPLIB:%OBJ%\forge.lib %OBJ%\*.obj || exit /b 1
copy /y data\Roboto-Medium.ttf "%OUT%\Forge\fonts\" >nul
if not exist "%OUT%\Forge\forge.ini" copy /y data\forge.ini "%OUT%\Forge\" >nul
rem licenses travel with the binaries (MIT / Apache 2.0 notices)
if not exist "%OUT%\Forge\licenses" mkdir "%OUT%\Forge\licenses"
copy /y LICENSE "%OUT%\Forge\licenses\LICENSE.txt" >nul
copy /y LICENSE-MHGU-Forge.txt "%OUT%\Forge\licenses\" >nul
copy /y THIRD_PARTY_NOTICES.md "%OUT%\Forge\licenses\" >nul
copy /y third_party\imgui\LICENSE.txt "%OUT%\Forge\licenses\LICENSE-imgui.txt" >nul
copy /y data\LICENSE-Roboto.txt "%OUT%\Forge\licenses\" >nul

rem example plugin: compiled to check it still builds, not installed
cl /nologo /O2 /MT /W4 /LD /Iinclude examples\hello\hello.c /Fo%OBJ%\hello.obj /Fe:build\examples\hello.dll /link /IMPLIB:%OBJ%\hello.lib || exit /b 1

rem HUD Fix mod (plugin with its own Dear ImGui, same version as forge.dll)
cl /nologo /O2 /MT /EHsc /std:c++20 /W3 /utf-8 /DNOMINMAX /Iinclude /Ithird_party\imgui /c /Fo%OBJ%\hudfix\ mods\HudFix\hudfix.cpp third_party\imgui\imgui.cpp third_party\imgui\imgui_draw.cpp third_party\imgui\imgui_tables.cpp third_party\imgui\imgui_widgets.cpp || exit /b 1
link /nologo /DLL /OUT:"%OUT%\HudFix\plugins\hudfix.dll" /IMPLIB:%OBJ%\hudfix.lib %OBJ%\hudfix\*.obj || exit /b 1
del /q "%OUT%\HudFix\plugins\hudfix.exp" 2>nul
copy /y LICENSE "%OUT%\HudFix\LICENSE.txt" >nul
copy /y LICENSE-MHGU-Forge.txt "%OUT%\HudFix\" >nul
copy /y third_party\imgui\LICENSE.txt "%OUT%\HudFix\LICENSE-imgui.txt" >nul
if not exist "%OUT%\HudFix\plugins\hudfix_layouts.ini" copy /y mods\HudFix\hudfix_layouts.ini "%OUT%\HudFix\plugins\" >nul

echo.
echo Built: %OUT%\Forge\forge.dll  and  %OUT%\HudFix\plugins\hudfix.dll
