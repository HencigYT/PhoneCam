@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
cd /d %~dp0..
if not exist obj\tools mkdir obj\tools
set CFLAGS=/nologo /O2 /EHsc /std:c++17 /utf-8 /W3 /MT /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN /Foobj\tools\
cl %CFLAGS% tools\selftest.cpp app\server.cpp app\decoder.cpp app\shmwriter.cpp app\h264patch.cpp app\install.cpp /Fe:obj\tools\selftest.exe /link mfplat.lib mfuuid.lib mf.lib mfreadwrite.lib mfsensorgroup.lib d3d11.lib ws2_32.lib iphlpapi.lib ole32.lib oleaut32.lib advapi32.lib shell32.lib
cl %CFLAGS% tools\vcamtest.cpp /Fe:obj\tools\vcamtest.exe /link mfplat.lib mfuuid.lib mf.lib mfreadwrite.lib ole32.lib
cl %CFLAGS% tools\camcheck.cpp /Fe:obj\tools\camcheck.exe /link mfplat.lib mfuuid.lib mf.lib mfreadwrite.lib strmiids.lib ole32.lib oleaut32.lib
