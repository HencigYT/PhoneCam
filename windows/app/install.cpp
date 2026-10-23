#include "install.h"
#include "../common/shared.h"
#include <shellapi.h>
#include <shlobj.h>
#include <mfapi.h>
#include <mfvirtualcamera.h>
#include <wrl/client.h>
#include <fstream>
#include <iterator>
#include <vector>

using Microsoft::WRL::ComPtr;

static const wchar_t kAppKey[] = L"SOFTWARE\\PhoneCam";
static const wchar_t kDllName[] = L"PhoneCamVCam.dll";
// Bumped whenever the set of registered cameras changes, so the setup reruns.
static const DWORD kSetupVersion = 2;

static std::wstring ClsidKey(int camera)
{
    return std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + pcam::kCameraClsidStrings[camera];
}

static std::wstring ServerKey(int camera)
{
    return ClsidKey(camera) + L"\\InprocServer32";
}

std::wstring ExePath()
{
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return path;
}

std::wstring ExeDir()
{
    std::wstring p = ExePath();
    return p.substr(0, p.find_last_of(L'\\'));
}

static std::wstring InstallDir()
{
    PWSTR pf = nullptr;
    std::wstring dir = L"C:\\Program Files";
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, nullptr, &pf)))
        dir = pf;
    CoTaskMemFree(pf);
    return dir + L"\\PhoneCam";
}

static std::wstring ReadRegString(const std::wstring& key, const wchar_t* value)
{
    wchar_t buf[1024];
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), value, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS)
        return L"";
    return buf;
}

static DWORD ReadRegDword(const std::wstring& key, const wchar_t* value)
{
    DWORD v = 0, size = sizeof(v);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), value, RRF_RT_REG_DWORD, nullptr, &v, &size) != ERROR_SUCCESS)
        return 0;
    return v;
}

// The camera DLL and adb are embedded in PhoneCam.exe (app.rc), so the exe works
// on its own when it is downloaded or sent to someone.
static bool EmbeddedFile(int id, const uint8_t*& data, DWORD& size)
{
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(id), (LPCWSTR)RT_RCDATA);
    HGLOBAL loaded = res ? LoadResource(nullptr, res) : nullptr;
    data = loaded ? (const uint8_t*)LockResource(loaded) : nullptr;
    size = res ? SizeofResource(nullptr, res) : 0;
    return data && size;
}

static bool FileEquals(const std::wstring& path, const uint8_t* data, DWORD size)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;
    std::vector<char> content((std::istreambuf_iterator<char>(f)), {});
    return content.size() == size && memcmp(content.data(), data, size) == 0;
}

static bool WriteFileBytes(const std::wstring& path, const uint8_t* data, DWORD size)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    DWORD written = 0;
    bool ok = WriteFile(h, data, size, &written, nullptr) && written == size;
    CloseHandle(h);
    return ok;
}

std::wstring ExtractAdb()
{
    static const struct
    {
        int id;
        const wchar_t* name;
    } files[] = { { kResAdb, L"adb.exe" }, { kResAdbWinApi, L"AdbWinApi.dll" }, { kResAdbWinUsbApi, L"AdbWinUsbApi.dll" } };
    wchar_t base[MAX_PATH];
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH))
        return L"";
    std::wstring dir = std::wstring(base) + L"\\PhoneCam";
    CreateDirectoryW(dir.c_str(), nullptr);
    for (auto& f : files)
    {
        const uint8_t* data;
        DWORD size;
        if (!EmbeddedFile(f.id, data, size))
            return L"";
        std::wstring path = dir + L"\\" + f.name;
        if (!FileEquals(path, data, size) && !WriteFileBytes(path, data, size))
            return L"";
    }
    return dir + L"\\adb.exe";
}

bool InstallNeeded()
{
    std::wstring installed = InstallDir() + L"\\" + kDllName;
    for (int i = 0; i < pcam::kCameraCount; i++)
        if (_wcsicmp(ReadRegString(ServerKey(i), nullptr).c_str(), installed.c_str()) != 0)
            return true;
    if (ReadRegDword(kAppKey, L"SetupVersion") != kSetupVersion)
        return true;
    const uint8_t* dll;
    DWORD dllSize;
    if (!EmbeddedFile(kResCameraDll, dll, dllSize) || !FileEquals(installed, dll, dllSize))
        return true;
    // The firewall rule is bound to the exe path.
    return _wcsicmp(ReadRegString(kAppKey, L"AppPath").c_str(), ExePath().c_str()) != 0;
}

static bool SetRegString(const std::wstring& key, const wchar_t* value, const std::wstring& data)
{
    return RegSetKeyValueW(HKEY_LOCAL_MACHINE, key.c_str(), value, REG_SZ, data.c_str(),
                           (DWORD)((data.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

static void RunHidden(std::wstring cmd)
{
    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
    {
        WaitForSingleObject(pi.hProcess, 15000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

static std::wstring Netsh()
{
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    return std::wstring(L"\"") + sys + L"\\netsh.exe\" advfirewall firewall ";
}

// System-lifetime virtual cameras stay registered when PhoneCam.exe is closed and
// across reboots, so apps like OBS always find them (they show a placeholder
// until the program runs). Creating the same camera again returns the existing one.
static HRESULT SetupCameras(bool remove)
{
    HRESULT result = S_OK;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);
    for (int i = 0; i < pcam::kCameraCount; i++)
    {
        ComPtr<IMFVirtualCamera> cam;
        HRESULT hr = MFCreateVirtualCamera(MFVirtualCameraType_SoftwareCameraSource, MFVirtualCameraLifetime_System,
                                           MFVirtualCameraAccess_AllUsers, pcam::kCameraNames[i],
                                           pcam::kCameraClsidStrings[i], nullptr, 0, &cam);
        if (SUCCEEDED(hr))
            hr = remove ? cam->Remove() : cam->Start(nullptr);
        if (cam)
            cam->Shutdown();
        if (FAILED(hr))
            result = hr;
    }
    MFShutdown();
    CoUninitialize();
    return result;
}

int DoInstall()
{
    std::wstring dir = InstallDir();
    std::wstring dst = dir + L"\\" + kDllName;
    const uint8_t* dll;
    DWORD dllSize;
    if (!EmbeddedFile(kResCameraDll, dll, dllSize))
        return 2;
    CreateDirectoryW(dir.c_str(), nullptr);
    if (!FileEquals(dst, dll, dllSize) && !WriteFileBytes(dst, dll, dllSize))
    {
        // The frame server may still have the old DLL loaded: move it aside.
        std::wstring old = dst + L"." + std::to_wstring(GetTickCount64()) + L".old";
        if (MoveFileExW(dst.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING))
            MoveFileExW(old.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
        if (!WriteFileBytes(dst, dll, dllSize))
            return 2;
    }
    for (int i = 0; i < pcam::kCameraCount; i++)
    {
        if (!SetRegString(ClsidKey(i), nullptr, pcam::kCameraNames[i]) || !SetRegString(ServerKey(i), nullptr, dst) ||
            !SetRegString(ServerKey(i), L"ThreadingModel", L"Both"))
            return 3;
    }
    // Start() can report an error for cameras that already exist; the window
    // shows whether Windows lists them, so this is not fatal.
    SetupCameras(false);

    RunHidden(Netsh() + L"delete rule name=\"PhoneCam\"");
    RunHidden(Netsh() + L"add rule name=\"PhoneCam\" dir=in action=allow program=\"" + ExePath() +
              L"\" enable=yes profile=any");
    SetRegString(kAppKey, L"AppPath", ExePath());
    DWORD version = kSetupVersion;
    RegSetKeyValueW(HKEY_LOCAL_MACHINE, kAppKey, L"SetupVersion", REG_DWORD, &version, sizeof(version));
    return 0;
}

int DoUninstall()
{
    SetupCameras(true);
    for (int i = 0; i < pcam::kCameraCount; i++)
        RegDeleteTreeW(HKEY_LOCAL_MACHINE, ClsidKey(i).c_str());
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, kAppKey);
    RunHidden(Netsh() + L"delete rule name=\"PhoneCam\"");
    std::wstring dst = InstallDir() + L"\\" + kDllName;
    if (!DeleteFileW(dst.c_str()))
        MoveFileExW(dst.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    RemoveDirectoryW(InstallDir().c_str());
    return 0;
}

bool RunElevated(const wchar_t* args, unsigned long& exitCode)
{
    std::wstring exe = ExePath();
    SHELLEXECUTEINFOW sei{ sizeof(sei) };
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = exe.c_str();
    sei.lpParameters = args;
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei) || !sei.hProcess)
        return false;
    WaitForSingleObject(sei.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    exitCode = code;
    return true;
}
