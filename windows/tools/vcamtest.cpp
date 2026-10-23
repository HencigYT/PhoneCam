// Opens the installed PhoneCam through the Windows frame server like a real
// app would and reads frames: vcamtest <seconds> [label] [width]
// Run several at once to test sharing, or in a row to test reopening.
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#include <stdio.h>
#include <stdlib.h>

using Microsoft::WRL::ComPtr;

static double NowMs()
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return c.QuadPart * 1000.0 / f.QuadPart;
}

int wmain(int argc, wchar_t** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    double seconds = argc > 1 ? _wtof(argv[1]) : 3;
    const wchar_t* label = argc > 2 ? argv[2] : L"A";
    UINT32 wantWidth = argc > 3 ? (UINT32)_wtoi(argv[3]) : 0;
    const wchar_t* camName = argc > 4 ? argv[4] : L"PhoneCam";
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);

    ComPtr<IMFAttributes> attrs;
    MFCreateAttributes(&attrs, 1);
    attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    IMFActivate** devices = nullptr;
    UINT32 count = 0;
    MFEnumDeviceSources(attrs.Get(), &devices, &count);
    ComPtr<IMFActivate> cam;
    for (UINT32 i = 0; i < count; i++)
    {
        wchar_t* name = nullptr;
        UINT32 len;
        devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &len);
        if (name && wcsstr(name, camName))
            cam = devices[i];
        CoTaskMemFree(name);
        devices[i]->Release();
    }
    CoTaskMemFree(devices);
    if (!cam)
    {
        wprintf(L"[%s] PhoneCam not found\n", label);
        return 2;
    }
    double t0 = NowMs();
    ComPtr<IMFMediaSource> source;
    HRESULT hr = cam->ActivateObject(IID_PPV_ARGS(&source));
    ComPtr<IMFSourceReader> reader;
    if (SUCCEEDED(hr))
        hr = MFCreateSourceReaderFromMediaSource(source.Get(), nullptr, &reader);
    if (FAILED(hr))
    {
        wprintf(L"[%s] open failed 0x%08X\n", label, (unsigned)hr);
        return 3;
    }
    if (wantWidth)
    {
        ComPtr<IMFMediaType> t;
        for (DWORD i = 0; SUCCEEDED(reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, i, &t)); i++)
        {
            UINT32 w, h;
            MFGetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, &w, &h);
            if (w == wantWidth)
            {
                hr = reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, t.Get());
                wprintf(L"[%s] set %ux%u hr=0x%08X\n", label, w, h, (unsigned)hr);
                break;
            }
            t.Reset();
        }
    }
    ComPtr<IMFMediaType> type;
    reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &type);
    UINT32 w = 0, h = 0;
    MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &w, &h);

    int frames = 0;
    double first = -1, start = NowMs();
    hr = S_OK;
    while (NowMs() - start < seconds * 1000)
    {
        DWORD idx, flags;
        LONGLONG ts;
        ComPtr<IMFSample> s;
        hr = reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &idx, &flags, &ts, &s);
        if (FAILED(hr))
            break;
        if (s)
        {
            if (first < 0)
                first = NowMs() - t0;
            frames++;
        }
    }
    double secs = (NowMs() - start) / 1000;
    wprintf(L"[%s] %ux%u: %d frames in %.1fs (%.1f fps), first frame after %.0f ms, result 0x%08X\n", label, w, h,
            frames, secs, frames / secs, first, (unsigned)hr);
    reader.Reset();
    source->Shutdown();
    return FAILED(hr) ? 4 : 0;
}
