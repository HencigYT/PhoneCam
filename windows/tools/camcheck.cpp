// Checks the installed virtual camera the way real apps see it: lists the
// cameras visible to Media Foundation and DirectShow, then pulls frames from
// "PhoneCam" through the Windows frame server.
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <dshow.h>
#include <wrl/client.h>
#include <stdio.h>
#include <string>

using Microsoft::WRL::ComPtr;

static double NowMs()
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return c.QuadPart * 1000.0 / f.QuadPart;
}

int wmain()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);

    printf("DirectShow video devices (OBS, Discord, Zoom):\n");
    ComPtr<ICreateDevEnum> devEnum;
    ComPtr<IEnumMoniker> monikers;
    if (SUCCEEDED(CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&devEnum))) &&
        devEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &monikers, 0) == S_OK)
    {
        ComPtr<IMoniker> m;
        while (monikers->Next(1, &m, nullptr) == S_OK)
        {
            ComPtr<IPropertyBag> bag;
            if (SUCCEEDED(m->BindToStorage(nullptr, nullptr, IID_PPV_ARGS(&bag))))
            {
                VARIANT v;
                VariantInit(&v);
                if (SUCCEEDED(bag->Read(L"FriendlyName", &v, nullptr)))
                    wprintf(L"  - %s\n", v.bstrVal);
                VariantClear(&v);
            }
            m.Reset();
        }
    }

    printf("Media Foundation video devices (Teams, Camera app, browsers):\n");
    ComPtr<IMFAttributes> attrs;
    MFCreateAttributes(&attrs, 1);
    attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    IMFActivate** devices = nullptr;
    UINT32 count = 0;
    MFEnumDeviceSources(attrs.Get(), &devices, &count);
    ComPtr<IMFActivate> phonecam;
    for (UINT32 i = 0; i < count; i++)
    {
        wchar_t* name = nullptr;
        UINT32 len = 0;
        devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &len);
        wprintf(L"  - %s\n", name ? name : L"?");
        if (name && wcsstr(name, L"PhoneCam"))
            phonecam = devices[i];
        CoTaskMemFree(name);
        devices[i]->Release();
    }
    CoTaskMemFree(devices);
    if (!phonecam)
    {
        printf("PhoneCam not found - is PhoneCam.exe running?\n");
        return 1;
    }

    ComPtr<IMFMediaSource> source;
    HRESULT hr = phonecam->ActivateObject(IID_PPV_ARGS(&source));
    ComPtr<IMFSourceReader> reader;
    if (SUCCEEDED(hr))
        hr = MFCreateSourceReaderFromMediaSource(source.Get(), nullptr, &reader);
    printf("open PhoneCam: hr=0x%08X\n", (unsigned)hr);
    if (FAILED(hr))
        return 1;

    ComPtr<IMFMediaType> type;
    reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &type);
    UINT32 w = 0, h = 0, n = 0, d = 1;
    MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &w, &h);
    MFGetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, &n, &d);
    printf("format: %ux%u @ %u fps\n", w, h, d ? n / d : 0);

    double start = NowMs();
    int frames = 0, changed = 0, last = -1;
    while (NowMs() - start < 5000)
    {
        DWORD idx, flags;
        LONGLONG ts;
        ComPtr<IMFSample> s;
        hr = reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &idx, &flags, &ts, &s);
        if (FAILED(hr))
        {
            printf("ReadSample failed 0x%08X\n", (unsigned)hr);
            break;
        }
        if (!s)
            continue;
        frames++;
        ComPtr<IMFMediaBuffer> b;
        s->ConvertToContiguousBuffer(&b);
        BYTE* p;
        DWORD len;
        b->Lock(&p, nullptr, &len);
        unsigned sum = 0;
        for (DWORD i = 0; i < w * h; i += 997)
            sum += p[i];
        b->Unlock();
        if ((int)sum != last)
            changed++;
        last = (int)sum;
    }
    double secs = (NowMs() - start) / 1000;
    printf("read %d frames in %.1fs = %.1f fps, %d distinct images%s\n", frames, secs, frames / secs, changed,
           changed <= 2 ? " (static: placeholder or no phone connected)" : "");
    source->Shutdown();
    return 0;
}
