@echo off
setlocal
set VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat
if not exist "%VCVARS%" set VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat
call "%VCVARS%" >nul || exit /b 1
cd /d "%~dp0"
if not exist dist mkdir dist
if not exist obj\vcam mkdir obj\vcam
if not exist obj\app mkdir obj\app

set CFLAGS=/nologo /O2 /EHsc /std:c++17 /utf-8 /W3 /MT /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN

echo == PhoneCamVCam.dll
cl %CFLAGS% /LD /Foobj\vcam\ vcam\*.cpp /Fe:obj\vcam\PhoneCamVCam.dll /link /DEF:vcam\vcam.def ^
  mfplat.lib mfuuid.lib ole32.lib advapi32.lib runtimeobject.lib gdi32.lib user32.lib || exit /b 1

rem The camera DLL and adb (USB mode) are embedded, PhoneCam.exe needs nothing next to it.
set PT=%LOCALAPPDATA%\Android\Sdk\platform-tools
if not exist "%PT%\adb.exe" (
  echo adb.exe not found in %PT%
  exit /b 1
)

echo == PhoneCam.exe
rc /nologo /i obj\vcam /i "%PT%" /fo obj\app\app.res app\app.rc || exit /b 1
cl %CFLAGS% /Foobj\app\ app\*.cpp obj\app\app.res /Fe:obj\app\PhoneCam.exe /link /SUBSYSTEM:WINDOWS ^
  mfplat.lib mfuuid.lib mf.lib mfsensorgroup.lib d3d11.lib ws2_32.lib iphlpapi.lib ole32.lib ^
  advapi32.lib user32.lib gdi32.lib shell32.lib comctl32.lib oleaut32.lib gdiplus.lib dwmapi.lib uxtheme.lib || exit /b 1

copy /y obj\app\PhoneCam.exe dist\ >nul 2>nul || echo (dist\PhoneCam.exe is running - fresh build: obj\app\PhoneCam.exe)
rem the self test loads the DLL from dist
copy /y obj\vcam\PhoneCamVCam.dll dist\ >nul
del /q dist\*.exp dist\*.lib dist\adb.exe dist\AdbWinApi.dll dist\AdbWinUsbApi.dll 2>nul
echo OK
