// End-to-end self test without a phone or admin rights:
// fake phone (pre-encoded MF H.264 stream) -> TCP -> Server/Decoder -> shared
// memory -> PhoneCamVCam.dll media source read through an IMFSourceReader.
#include "../app/server.h"
#include "../app/decoder.h"
#include "../app/h264patch.h"
#include "../app/lang.h"

bool lang::g_english = false;
#include <mfreadwrite.h>
#include <codecapi.h>
#include <strmif.h>
#include <stdio.h>
#include <atomic>
#include <thread>

static int W = 1920, H = 1080, FPS = 60, SECONDS = 6;
static std::mutex g_sendLock;

static void PutBe32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static void PutBe64(uint8_t* p, int64_t v)
{
    PutBe32(p, (uint32_t)((uint64_t)v >> 32));
    PutBe32(p + 4, (uint32_t)v);
}

static bool SendPacket(SOCKET s, uint8_t type, const std::vector<uint8_t>& payload)
{
    std::vector<uint8_t> b(5 + payload.size());
    b[0] = type;
    PutBe32(&b[1], (uint32_t)payload.size());
    memcpy(b.data() + 5, payload.data(), payload.size());
    std::lock_guard<std::mutex> lk(g_sendLock);
    return send(s, (const char*)b.data(), (int)b.size(), 0) == (int)b.size();
}

struct EncodedFrame
{
    std::vector<uint8_t> data;
    bool key;
};

static std::vector<EncodedFrame> EncodeClip()
{
    std::vector<EncodedFrame> frames;
    MFT_REGISTER_TYPE_INFO out{ MFMediaType_Video, MFVideoFormat_H264 };
    IMFActivate** acts = nullptr;
    UINT32 n = 0;
    ComPtr<IMFTransform> enc;
    MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_SYNCMFT, nullptr, &out, &acts, &n);
    for (UINT32 i = 0; i < n; i++)
    {
        if (!enc) acts[i]->ActivateObject(IID_PPV_ARGS(&enc));
        acts[i]->Release();
    }
    CoTaskMemFree(acts);
    if (!enc) return frames;
    ComPtr<ICodecAPI> api;
    if (SUCCEEDED(enc.As(&api)))
    {
        VARIANT v; VariantInit(&v); v.vt = VT_UI4; v.ulVal = TRUE;
        api->SetValue(&CODECAPI_AVLowLatencyMode, &v);
        v.ulVal = 0;
        if (!GetEnvironmentVariableW(L"SELFTEST_DEFAULT_B", nullptr, 0))
            api->SetValue(&CODECAPI_AVEncMPVDefaultBPictureCount, &v);
    }
    ComPtr<IMFMediaType> ot, it;
    MFCreateMediaType(&ot);
    ot->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    ot->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    MFSetAttributeSize(ot.Get(), MF_MT_FRAME_SIZE, W, H);
    MFSetAttributeRatio(ot.Get(), MF_MT_FRAME_RATE, FPS, 1);
    ot->SetUINT32(MF_MT_AVG_BITRATE, 20000000);
    ot->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    enc->SetOutputType(0, ot.Get(), 0);
    MFCreateMediaType(&it);
    it->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    it->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    MFSetAttributeSize(it.Get(), MF_MT_FRAME_SIZE, W, H);
    MFSetAttributeRatio(it.Get(), MF_MT_FRAME_RATE, FPS, 1);
    it->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (FAILED(enc->SetInputType(0, it.Get(), 0))) { printf("encoder: SetInputType failed\n"); return frames; }
    enc->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    MFT_OUTPUT_STREAM_INFO info{};
    enc->GetOutputStreamInfo(0, &info);
    bool provides = (info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;

    for (int i = 0; i < SECONDS * FPS; i++)
    {
        ComPtr<IMFMediaBuffer> buf;
        MFCreateMemoryBuffer(W * H * 3 / 2, &buf);
        BYTE* p; buf->Lock(&p, nullptr, nullptr);
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                p[y * W + x] = (uint8_t)(x / 8 + i * 4); // moving gradient
        memset(p + W * H, 128, W * H / 2);
        buf->Unlock();
        buf->SetCurrentLength(W * H * 3 / 2);
        ComPtr<IMFSample> in;
        MFCreateSample(&in);
        in->AddBuffer(buf.Get());
        in->SetSampleTime((LONGLONG)i * 10000000 / FPS);
        in->SetSampleDuration(10000000 / FPS);
        enc->ProcessInput(0, in.Get(), 0);
        for (;;)
        {
            MFT_OUTPUT_DATA_BUFFER ob{};
            ComPtr<IMFSample> mine;
            if (!provides)
            {
                ComPtr<IMFMediaBuffer> ob2;
                MFCreateMemoryBuffer(std::max<DWORD>(info.cbSize, W * H * 2), &ob2);
                MFCreateSample(&mine);
                mine->AddBuffer(ob2.Get());
                ob.pSample = mine.Get();
            }
            DWORD st = 0;
            HRESULT hr = enc->ProcessOutput(0, 1, &ob, &st);
            if (ob.pEvents) ob.pEvents->Release();
            if (hr != S_OK) break;
            ComPtr<IMFSample> outS;
            if (provides) outS.Attach(ob.pSample); else outS = mine;
            ComPtr<IMFMediaBuffer> cb;
            outS->ConvertToContiguousBuffer(&cb);
            BYTE* d; DWORD len;
            cb->Lock(&d, nullptr, &len);
            frames.push_back({ std::vector<uint8_t>(d, d + len),
                               MFGetAttributeUINT32(outS.Get(), MFSampleExtension_CleanPoint, 0) != 0 });
            cb->Unlock();
        }
    }
    return frames;
}

static void FakePhone(const std::vector<EncodedFrame>* clip)
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(proto::kTestStreamPort);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    BOOL nd = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&nd, sizeof(nd));
    if (connect(s, (sockaddr*)&a, sizeof(a)) != 0) { printf("phone: connect failed\n"); return; }

    const char name[] = "Fake S23";
    std::vector<uint8_t> hello(8 + sizeof(name) - 1 + 1);
    PutBe32(&hello[0], proto::kMagic);
    hello[4] = 0; hello[5] = 1;
    hello[6] = 0; hello[7] = (uint8_t)(sizeof(name) - 1);
    memcpy(&hello[8], name, sizeof(name) - 1);
    hello.back() = 0;
    send(s, (const char*)hello.data(), (int)hello.size(), 0);

    uint8_t th[5];
    recv(s, (char*)th, 5, MSG_WAITALL);
    uint32_t clen = (uint32_t)th[1] << 24 | th[2] << 16 | th[3] << 8 | th[4];
    std::vector<uint8_t> caps(clen);
    recv(s, (char*)caps.data(), (int)clen, MSG_WAITALL);
    printf("phone: connected, PC codecs mask=%d, PC name=%.*s\n", caps[0], (int)clen - 1, (const char*)caps.data() + 1);

    std::thread reader([&] {
        for (;;)
        {
            uint8_t h[5];
            if (recv(s, (char*)h, 5, MSG_WAITALL) != 5) break;
            uint32_t len = (uint32_t)h[1] << 24 | h[2] << 16 | h[3] << 8 | h[4];
            std::vector<uint8_t> p(len);
            if (len && recv(s, (char*)p.data(), (int)len, MSG_WAITALL) != (int)len) break;
            if (h[0] == proto::kSet)
                printf("  phone got settings from PC: %.*s", (int)len, (const char*)p.data());
            if (h[0] == proto::kPing)
            {
                static int n = 0;
                int64_t sentAt = 0;
                for (int i = 0; i < 8; i++) sentAt = sentAt << 8 | p[i];
                if (n++ < 4) printf("  ping one-way PC->phone: %.2f ms\n", (NowUs() - sentAt) / 1000.0);
                std::vector<uint8_t> pong(16);
                memcpy(pong.data(), p.data(), 8);
                PutBe64(&pong[8], NowUs());
                SendPacket(s, proto::kPong, pong);
            }
        }
    });

    std::vector<uint8_t> cfg(20);
    cfg[0] = 0;
    PutBe32(&cfg[1], W); PutBe32(&cfg[5], H); PutBe32(&cfg[9], FPS); PutBe32(&cfg[13], 20000000);
    cfg[17] = 0; cfg[18] = 0; cfg[19] = 1;
    SendPacket(s, proto::kConfig, cfg);
    std::string info = u8R"(battery=87
charging=1
camera=0
width=1920
height=1080
fps=60
codec=0
bitrate=0
stab=0
torch=0
flash=1
zoom=1.00
zoommin=0.60
zoommax=10.00
cam=0|Hátsó #0, 6.3 mm|3840x2160,1920x1080,1280x720|60,30
cam=1|Előlapi #1|1920x1080|30
)";
    SendPacket(s, proto::kInfo, std::vector<uint8_t>(info.begin(), info.end()));

    int64_t start = NowUs();
    for (size_t i = 0; i < clip->size(); i++)
    {
        int64_t due = start + (int64_t)i * 1000000 / FPS;
        while (NowUs() < due) Sleep(1);
        const EncodedFrame& f = (*clip)[i];
        std::vector<uint8_t> frame(9 + f.data.size());
        frame[0] = f.key ? 1 : 0;
        PutBe64(&frame[1], NowUs()); // "capture" time on the phone clock
        memcpy(&frame[9], f.data.data(), f.data.size());
        SendPacket(s, proto::kFrame, frame);
    }
    printf("phone: sent %zu frames in %.2fs\n", clip->size(), (NowUs() - start) / 1e6);
    shutdown(s, SD_BOTH);
    reader.join();
    closesocket(s);
}

typedef HRESULT(STDAPICALLTYPE* GetClassObjectFn)(REFCLSID, REFIID, void**);

int wmain(int argc, wchar_t** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc == 4 && wcscmp(argv[1], L"patch") == 0)
    {
        FILE* f = nullptr;
        if (_wfopen_s(&f, argv[2], L"rb")) return 1;
        std::vector<uint8_t> in(1 << 20);
        in.resize(fread(in.data(), 1, in.size(), f));
        fclose(f);
        std::vector<uint8_t> out;
        bool changed = PatchH264NoReorder(in.data(), in.size(), out);
        printf("patch changed=%d %zu -> %zu bytes\n", changed, in.size(), out.size());
        if (_wfopen_s(&f, argv[3], L"wb")) return 1;
        fwrite(changed ? out.data() : in.data(), 1, changed ? out.size() : in.size(), f);
        fclose(f);
        return 0;
    }
    if (argc >= 4)
    {
        W = _wtoi(argv[1]);
        H = _wtoi(argv[2]);
        FPS = _wtoi(argv[3]);
    }
    SetEnvironmentVariableW(L"PHONECAM_TEST_LOCAL", L"1");
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);

    // "selftest phone W H FPS": only the fake phone, for testing another receiver (e.g. the Linux jar).
    if (argc >= 5 && wcscmp(argv[1], L"phone") == 0)
    {
        W = _wtoi(argv[2]);
        H = _wtoi(argv[3]);
        FPS = _wtoi(argv[4]);
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        std::vector<EncodedFrame> clip = EncodeClip();
        printf("encoded %zu frames\n", clip.size());
        FakePhone(&clip);
        return 0;
    }

    int64_t t0 = NowUs();
    std::vector<EncodedFrame> clip = EncodeClip();
    size_t bytes = 0;
    for (auto& f : clip) bytes += f.data.size();
    printf("encoded %zu frames %dx%d@%d in %.1fs, avg %.1f KB, first key=%d\n", clip.size(), W, H, FPS,
           (NowUs() - t0) / 1e6, clip.empty() ? 0 : bytes / 1024.0 / clip.size(), clip.empty() ? 0 : clip[0].key);
    if (clip.empty()) return 1;
    FILE* f = nullptr;
    if (_wfopen_s(&f, L"first.h264", L"wb") == 0) { fwrite(clip[0].data.data(), 1, clip[0].data.size(), f); fclose(f); }
    std::vector<uint8_t> patched;
    if (PatchH264NoReorder(clip[0].data.data(), clip[0].data.size(), patched) &&
        _wfopen_s(&f, L"patched.h264", L"wb") == 0) { fwrite(patched.data(), 1, patched.size(), f); fclose(f); }

    Server server;
    std::wstring err;
    if (!server.Start(err)) { wprintf(L"server: %s\n", err.c_str()); return 1; }
    std::thread phone(FakePhone, &clip);
    Sleep(1500);

    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dll = exe;
    dll = dll.substr(0, dll.find_last_of(L'\\')) + L"\\..\\..\\dist\\PhoneCamVCam.dll";
    HMODULE m = LoadLibraryW(dll.c_str());
    if (!m) { printf("LoadLibrary failed %lu\n", GetLastError()); phone.join(); return 1; }
    auto getClass = (GetClassObjectFn)GetProcAddress(m, "DllGetClassObject");
    ComPtr<IClassFactory> cf;
    ComPtr<IMFActivate> act;
    ComPtr<IMFMediaSource> src;
    HRESULT hr = getClass(pcam::kCameraClsids[0], IID_PPV_ARGS(&cf));
    if (SUCCEEDED(hr)) hr = cf->CreateInstance(nullptr, IID_PPV_ARGS(&act));
    if (SUCCEEDED(hr)) hr = act->ActivateObject(IID_PPV_ARGS(&src));
    ComPtr<IMFSourceReader> reader;
    if (SUCCEEDED(hr)) hr = MFCreateSourceReaderFromMediaSource(src.Get(), nullptr, &reader);
    printf("source + reader: hr=0x%08X\n", (unsigned)hr);
    if (FAILED(hr)) { phone.join(); return 1; }

    auto readSome = [&](int count, const char* label) {
        ComPtr<IMFMediaType> cur;
        reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur);
        UINT32 w, h, n, d;
        MFGetAttributeSize(cur.Get(), MF_MT_FRAME_SIZE, &w, &h);
        MFGetAttributeRatio(cur.Get(), MF_MT_FRAME_RATE, &n, &d);
        int64_t start = NowUs();
        int fresh = 0, prevY = -1;
        for (int i = 0; i < count; i++)
        {
            DWORD idx, flags;
            LONGLONG ts;
            ComPtr<IMFSample> s;
            hr = reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &idx, &flags, &ts, &s);
            if (FAILED(hr) || !s) { printf("  read failed hr=0x%08X flags=%lu\n", (unsigned)hr, flags); return; }
            ComPtr<IMFMediaBuffer> b;
            s->ConvertToContiguousBuffer(&b);
            BYTE* p; DWORD len;
            b->Lock(&p, nullptr, &len);
            int y = p[(h / 2) * w + w / 2];
            b->Unlock();
            if (y != prevY) fresh++;
            prevY = y;
        }
        double secs = (NowUs() - start) / 1e6;
        printf("%s: %ux%u@%u, %d samples in %.2fs (%.1f/s), %d changed\n", label, w, h, n / d, count, secs,
               count / secs, fresh);
    };
    auto pick = [&](UINT32 want, bool yuy2) {
        ComPtr<IMFMediaType> t;
        for (DWORD i = 0; SUCCEEDED(reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, i, &t)); i++)
        {
            UINT32 w, h;
            GUID sub;
            MFGetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, &w, &h);
            t->GetGUID(MF_MT_SUBTYPE, &sub);
            if (w == want && (sub == MFVideoFormat_YUY2) == yuy2)
                return SUCCEEDED(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, t.Get()));
            t.Reset();
        }
        return false;
    };

    std::atomic<bool> polling{ true };
    std::thread poller([&] { while (polling) { Stats st = server.GetStats(); if (st.connected) printf("  poll: in=%.1f decode=%.2f errors=%d err='%ls'\n", st.inFps, st.decodeMs, st.decodeErrors, st.error.c_str()); Sleep(1000); } });
    readSome(FPS * 2, "default type");
    {
        uint64_t serial;
        PhoneInfo pi = server.GetPhoneInfo(serial);
        wprintf(L"  phone info: valid=%d battery=%d%%%s cameras=%zu [%s: %zu res, %zu fps] zoom %.2f..%.2f\n", pi.valid,
                pi.battery, pi.charging ? L" (charging)" : L"", pi.cameras.size(),
                pi.cameras.empty() ? L"-" : pi.cameras[0].label.c_str(),
                pi.cameras.empty() ? 0 : pi.cameras[0].resolutions.size(), pi.cameras.empty() ? 0 : pi.cameras[0].fps.size(),
                pi.zoomMin, pi.zoomMax);
        printf("  send settings: %d\n", server.SendSettings("torch=1\nzoom=2.50\n"));
    }
    // Two cameras at once (e.g. Discord + OBS): both must get every frame.
    {
        ComPtr<IClassFactory> cf2;
        ComPtr<IMFActivate> act2;
        ComPtr<IMFMediaSource> src2;
        ComPtr<IMFSourceReader> reader2;
        HRESULT h2 = getClass(pcam::kCameraClsids[1], IID_PPV_ARGS(&cf2));
        if (SUCCEEDED(h2)) h2 = cf2->CreateInstance(nullptr, IID_PPV_ARGS(&act2));
        if (SUCCEEDED(h2)) h2 = act2->ActivateObject(IID_PPV_ARGS(&src2));
        if (SUCCEEDED(h2)) h2 = MFCreateSourceReaderFromMediaSource(src2.Get(), nullptr, &reader2);
        if (SUCCEEDED(h2))
        {
            std::atomic<int> n2{ 0 };
            std::thread t2([&] {
                for (int i = 0; i < FPS; i++)
                {
                    DWORD idx, flags; LONGLONG ts; ComPtr<IMFSample> s;
                    if (FAILED(reader2->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &idx, &flags, &ts, &s))) break;
                    if (s) n2++;
                }
            });
            readSome(FPS, "camera 1 while camera 2 runs");
            t2.join();
            int c2w = 0, c2h = 0;
            bool a1 = server.ClientActive(1, c2w, c2h);
            printf("  camera 2 got %d frames, active=%d (%dx%d)\n", n2.load(), a1, c2w, c2h);
            src2->Shutdown();
        }
        else
            printf("  camera 2 open failed 0x%08X\n", (unsigned)h2);
    }
    auto stats = [&] {
        Stats st = server.GetStats();
        wprintf(L"  server: connected=%d %dx%d@%d in=%.1ffps %.2fMbps decode=%.2fms (%s) latency=%.1fms rtt=%.2fms offset=%.2fms errors=%d\n",
                st.connected, st.width, st.height, st.fps, st.inFps, st.inMbps, st.decodeMs,
                st.hardwareDecode ? L"GPU" : L"CPU", st.latencyMs, st.rttMs, st.clockOffsetMs, st.decodeErrors);
    };
    stats();
    if (pick(1280, false))
        readSome(FPS, "1280x720 NV12 (scaled)");
    if (pick(1280, true))
        readSome(FPS / 2, "1280x720 YUY2 (scaled)");
    stats();
    int cw = 0, ch = 0;
    bool active = server.ClientActive(0, cw, ch);
    printf("client active: %d (%dx%d)\n", active, cw, ch);

    polling = false;
    poller.join();
    phone.join();
    src->Shutdown();
    reader.Reset();
    server.Stop();
    printf("done\n");
    return 0;
}
