#pragma once
#include <string>

// One-time machine setup (needs admin): copies the camera DLL somewhere the
// frame server service can read it, registers it in HKLM and opens the firewall.
std::wstring ExePath();
std::wstring ExeDir();
bool InstallNeeded();
int DoInstall();
int DoUninstall();
bool RunElevated(const wchar_t* args, unsigned long& exitCode);
// Unpacks the embedded adb (USB mode) to %LOCALAPPDATA%\PhoneCam; returns adb.exe path or "".
std::wstring ExtractAdb();

// Resource ids of the files embedded in PhoneCam.exe (app.rc).
enum
{
    kResCameraDll = 101,
    kResAdb = 102,
    kResAdbWinApi = 103,
    kResAdbWinUsbApi = 104,
};
