#include <wrl/module.h>
#include <string>
#include "source.h"

using namespace Microsoft::WRL;

CoCreatableClass(Activator1);
CoCreatableClass(Activator2);
CoCreatableClass(Activator3);

static HMODULE g_module;

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
        g_module = module;
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** object)
{
    return Module<InProc>::GetModule().GetClassObject(clsid, riid, object);
}

STDAPI DllCanUnloadNow()
{
    return Module<InProc>::GetModule().Terminate() ? S_OK : S_FALSE;
}

static std::wstring ClsidKey(int camera)
{
    return std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + pcam::kCameraClsidStrings[camera];
}

static LSTATUS SetString(const std::wstring& key, const wchar_t* value, const std::wstring& data)
{
    return RegSetKeyValueW(HKEY_LOCAL_MACHINE, key.c_str(), value, REG_SZ, data.c_str(),
                           (DWORD)((data.size() + 1) * sizeof(wchar_t)));
}

// Machine-wide registration: the frame server service only reads HKLM.
STDAPI DllRegisterServer()
{
    wchar_t path[MAX_PATH];
    if (!GetModuleFileNameW(g_module, path, MAX_PATH))
        return HRESULT_FROM_WIN32(GetLastError());
    LSTATUS st = ERROR_SUCCESS;
    for (int i = 0; i < pcam::kCameraCount && st == ERROR_SUCCESS; i++)
    {
        std::wstring key = ClsidKey(i);
        st = SetString(key, nullptr, pcam::kCameraNames[i]);
        if (st == ERROR_SUCCESS)
            st = SetString(key + L"\\InprocServer32", nullptr, path);
        if (st == ERROR_SUCCESS)
            st = SetString(key + L"\\InprocServer32", L"ThreadingModel", L"Both");
    }
    return HRESULT_FROM_WIN32(st);
}

STDAPI DllUnregisterServer()
{
    for (int i = 0; i < pcam::kCameraCount; i++)
        RegDeleteTreeW(HKEY_LOCAL_MACHINE, ClsidKey(i).c_str());
    return S_OK;
}
