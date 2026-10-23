#include "server.h"
#include "decoder.h"
#include "lang.h"
#include "install.h"
#include <iphlpapi.h>
#include <mstcpip.h>
#include <algorithm>
#include <deque>

static std::mutex g_writerLock;

int64_t NowUs()
{
    static LARGE_INTEGER freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (int64_t)(c.QuadPart / freq.QuadPart * 1000000 + (c.QuadPart % freq.QuadPart) * 1000000 / freq.QuadPart);
}

static uint16_t Be16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t Be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static int64_t Be64(const uint8_t* p) { return (int64_t)((uint64_t)Be32(p) << 32 | Be32(p + 4)); }
static void PutBe64(uint8_t* p, int64_t v)
{
    for (int i = 7; i >= 0; i--, v >>= 8)
        p[i] = (uint8_t)(v & 0xFF);
}

static std::wstring FromUtf8(const char* s, int len)
{
    if (len <= 0)
        return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s, len, nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, len, &w[0], n);
    return w;
}

static std::string ToUtf8(const std::wstring& w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

static std::wstring ComputerName()
{
    wchar_t name[256];
    DWORD size = 256;
    if (GetComputerNameExW(ComputerNameDnsHostname, name, &size))
        return name;
    return L"PC";
}

static bool RecvAll(SOCKET s, void* buffer, size_t size)
{
    char* p = (char*)buffer;
    while (size > 0)
    {
        int n = recv(s, p, (int)std::min<size_t>(size, 1 << 20), 0);
        if (n <= 0)
            return false;
        p += n;
        size -= n;
    }
    return true;
}

bool Server::Start(std::wstring& error)
{
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    _codecs = Decoder::SupportedCodecs();

    _listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    BOOL exclusive = TRUE;
    setsockopt(_listen, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&exclusive, sizeof(exclusive));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(pcam::UseLocalNames() ? proto::kTestStreamPort : proto::kStreamPort);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(_listen, (sockaddr*)&addr, sizeof(addr)) != 0 || listen(_listen, 4) != 0)
    {
        error = lang::g_english
                    ? L"TCP port " + std::to_wstring(proto::kStreamPort) + L" is in use (is PhoneCam already running?)"
                    : L"A " + std::to_wstring(proto::kStreamPort) + L" TCP port foglalt (fut már egy PhoneCam?)";
        closesocket(_listen);
        _listen = INVALID_SOCKET;
        return false;
    }
    _running = true;
    _acceptThread = std::thread(&Server::AcceptLoop, this);
    // The self test must not attract real phones on the network.
    if (!pcam::UseLocalNames())
    {
        _beaconThread = std::thread(&Server::BeaconLoop, this);
        _adbThread = std::thread(&Server::AdbLoop, this);
    }
    return true;
}

void Server::Stop()
{
    if (!_running.exchange(false))
        return;
    closesocket(_listen);
    SOCKET c = _client.exchange(INVALID_SOCKET);
    if (c != INVALID_SOCKET)
        shutdown(c, SD_BOTH);
    for (std::thread* t : { &_acceptThread, &_clientThread, &_beaconThread, &_adbThread })
        if (t->joinable())
            t->join();
}

Stats Server::GetStats()
{
    std::lock_guard<std::mutex> lk(_statsLock);
    return _stats;
}

std::vector<std::wstring> Server::LocalAddresses()
{
    std::lock_guard<std::mutex> lk(_statsLock);
    return _addresses;
}

bool Server::ClientActive(int camera, int& width, int& height)
{
    std::lock_guard<std::mutex> lk(g_writerLock);
    return _writer.ClientActive(camera, width, height);
}

PhoneInfo Server::GetPhoneInfo(uint64_t& serial)
{
    std::lock_guard<std::mutex> lk(_statsLock);
    serial = _phoneSerial;
    return _phone;
}

bool Server::SendSettings(const std::string& settings)
{
    SOCKET s = _client;
    if (s == INVALID_SOCKET || !GetStats().connected)
        return false;
    return Send(s, proto::kSet, settings.data(), (uint32_t)settings.size());
}

static std::vector<std::string> Split(const std::string& s, char sep)
{
    std::vector<std::string> out;
    size_t start = 0;
    for (;;)
    {
        size_t end = s.find(sep, start);
        out.push_back(s.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (end == std::string::npos)
            return out;
        start = end + 1;
    }
}

// "key=value" lines; camera entries: cam=<id>|<label>|<WxH,...>|<fps,...>
void Server::ParseInfo(const uint8_t* data, size_t size)
{
    PhoneInfo info;
    info.valid = true;
    for (const std::string& line : Split(std::string((const char*)data, size), '\n'))
    {
        size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        std::string key = line.substr(0, eq), value = line.substr(eq + 1);
        int n = atoi(value.c_str());
        if (key == "battery")
            info.battery = n;
        else if (key == "charging")
            info.charging = n != 0;
        else if (key == "camera")
            info.camera = value;
        else if (key == "width")
            info.width = n;
        else if (key == "height")
            info.height = n;
        else if (key == "fps")
            info.fps = n;
        else if (key == "codec")
            info.codec = n;
        else if (key == "bitrate")
            info.bitrate = n;
        else if (key == "stab")
            info.stabilization = n != 0;
        else if (key == "torch")
            info.torch = n != 0;
        else if (key == "flash")
            info.hasFlash = n != 0;
        else if (key == "zoom")
            info.zoom = atof(value.c_str());
        else if (key == "zoommin")
            info.zoomMin = atof(value.c_str());
        else if (key == "zoommax")
            info.zoomMax = atof(value.c_str());
        else if (key == "cam")
        {
            std::vector<std::string> parts = Split(value, '|');
            if (parts.size() < 4)
                continue;
            PhoneCamera cam;
            cam.id = parts[0];
            cam.label = FromUtf8(parts[1].data(), (int)parts[1].size());
            for (const std::string& r : Split(parts[2], ','))
            {
                int w = 0, h = 0;
                if (sscanf_s(r.c_str(), "%dx%d", &w, &h) == 2)
                    cam.resolutions.push_back({ w, h });
            }
            for (const std::string& f : Split(parts[3], ','))
                if (atoi(f.c_str()) > 0)
                    cam.fps.push_back(atoi(f.c_str()));
            info.cameras.push_back(cam);
        }
    }
    std::lock_guard<std::mutex> lk(_statsLock);
    _phone = info;
    _phoneSerial++;
}

bool Server::GetPreview(std::vector<uint32_t>& pixels, int& width, int& height, uint64_t& serial)
{
    std::lock_guard<std::mutex> lk(_previewLock);
    if (serial == _previewSerial)
        return false;
    pixels = _preview;
    width = _previewW;
    height = _previewH;
    serial = _previewSerial;
    return true;
}

void Server::AcceptLoop()
{
    while (_running)
    {
        sockaddr_in addr{};
        int len = sizeof(addr);
        SOCKET s = accept(_listen, (sockaddr*)&addr, &len);
        if (s == INVALID_SOCKET)
        {
            if (!_running)
                break;
            Sleep(50);
            continue;
        }
        // The newest connection wins (e.g. the phone reconnected after a Wi-Fi hiccup).
        SOCKET old = _client.exchange(INVALID_SOCKET);
        if (old != INVALID_SOCKET)
            shutdown(old, SD_BOTH);
        if (_clientThread.joinable())
            _clientThread.join();
        if (!_running)
        {
            closesocket(s);
            break;
        }
        wchar_t ip[64] = L"?";
        InetNtopW(AF_INET, &addr.sin_addr, ip, 64);
        _client = s;
        _clientThread = std::thread(&Server::ClientLoop, this, s, std::wstring(ip));
    }
}

bool Server::Send(SOCKET s, uint8_t type, const void* payload, uint32_t size)
{
    uint8_t header[5] = { type, (uint8_t)(size >> 24), (uint8_t)(size >> 16), (uint8_t)(size >> 8), (uint8_t)size };
    std::lock_guard<std::mutex> lk(_sendLock);
    if (send(s, (const char*)header, 5, 0) != 5)
        return false;
    return size == 0 || send(s, (const char*)payload, (int)size, 0) == (int)size;
}

void Server::ClientLoop(SOCKET s, std::wstring address)
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    BOOL noDelay = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&noDelay, sizeof(noDelay));
    int rcvBuf = 4 << 20;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char*)&rcvBuf, sizeof(rcvBuf));
    // The phone sends frames and answers pings constantly; 5 s of silence means it is gone.
    DWORD timeout = 5000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));

    // Hello: magic u32, version u16, name (u16 length + UTF-8), link u8 (1 = USB)
    uint8_t hello[8];
    std::wstring phoneName;
    bool usb = false;
    bool ok = RecvAll(s, hello, 8) && Be32(hello) == proto::kMagic;
    if (ok)
    {
        std::string name(Be16(hello + 6), '\0');
        uint8_t link = 0;
        ok = RecvAll(s, &name[0], name.size()) && RecvAll(s, &link, 1);
        phoneName = FromUtf8(name.data(), (int)name.size());
        usb = link == 1;
    }
    if (ok)
    {
        std::string caps(1, (char)_codecs);
        caps += ToUtf8(ComputerName());
        ok = Send(s, proto::kCaps, caps.data(), (uint32_t)caps.size());
    }
    if (!ok)
    {
        SOCKET expected = s;
        _client.compare_exchange_strong(expected, INVALID_SOCKET);
        closesocket(s);
        CoUninitialize();
        return;
    }

    {
        std::lock_guard<std::mutex> lk(_statsLock);
        _stats = Stats{};
        _stats.connected = true;
        _stats.usb = usb;
        _stats.phoneName = phoneName;
        _stats.phoneAddress = usb ? L"USB" : address;
    }
    _clockValid = false;
    _latencyAvg = -1;

    std::atomic<bool> alive{ true };
    std::thread pinger([&] {
        while (alive && _running)
        {
            uint8_t p[8];
            PutBe64(p, NowUs());
            if (!Send(s, proto::kPing, p, 8))
                break;
            for (int i = 0; i < 10 && alive; i++)
                Sleep(100);
        }
    });

    Decoder decoder;
    bool needKey = true;
    int64_t lastKeyRequest = 0;
    int64_t statsAt = NowUs();
    std::deque<double> rtts; // recent round trips, median is shown
    std::vector<uint8_t> buf;
    auto requestKey = [&] {
        int64_t now = NowUs();
        if (now - lastKeyRequest > 300000)
        {
            lastKeyRequest = now;
            Send(s, proto::kRequestKey, nullptr, 0);
        }
    };
    FrameCallback onFrame = [this](const uint8_t* y, int yp, const uint8_t* uv, int uvp, int w, int h, int64_t pts,
                                   bool fullRange) { OnFrame(y, yp, uv, uvp, w, h, pts, fullRange); };

    for (;;)
    {
        uint8_t th[5];
        if (!RecvAll(s, th, 5))
            break;
        uint32_t len = Be32(th + 1);
        if (len > (64u << 20))
            break;
        buf.resize(len);
        if (len && !RecvAll(s, buf.data(), len))
            break;
        const uint8_t* p = buf.data();

        switch (th[0])
        {
        case proto::kConfig:
        {
            if (len < 20)
                break;
            int codec = p[0], w = (int)Be32(p + 1), h = (int)Be32(p + 5), fps = (int)Be32(p + 9);
            int bitrate = (int)Be32(p + 13), rotation = Be16(p + 17);
            std::wstring error;
            bool ready = decoder.Init(codec, w, h, fps, error);
            _rotation = rotation;
            _fps = fps;
            needKey = true;
            lastKeyRequest = 0;
            requestKey();
            std::lock_guard<std::mutex> lk(_statsLock);
            _stats.codec = codec;
            _stats.width = w;
            _stats.height = h;
            _stats.fps = fps;
            _stats.bitrate = bitrate;
            _stats.rotation = rotation;
            _stats.hardwareDecode = ready && decoder.IsHardware();
            _stats.error = ready ? L"" : error;
            break;
        }
        case proto::kFrame:
        {
            if (len < 9 || !decoder.IsReady())
                break;
            bool key = (p[0] & 1) != 0;
            int64_t pts = Be64(p + 1);
            _frameCounter++;
            _byteCounter += len;
            if (needKey && !key)
            {
                requestKey();
                break;
            }
            needKey = false;
            int64_t t0 = NowUs();
            if (!decoder.Decode(p + 9, len - 9, pts, key, onFrame))
            {
                // Corrupt stream or decoder hiccup: start over from the next key frame.
                std::lock_guard<std::mutex> lk(_statsLock);
                _stats.decodeErrors++;
                wchar_t msg[64];
                swprintf_s(msg, lang::tr(L"utolsó dekódolási hiba: 0x%08X", L"last decoding error: 0x%08X"), (unsigned)decoder.LastError());
                _stats.error = msg;
                std::wstring error;
                decoder.Init(_stats.codec, _stats.width, _stats.height, _stats.fps, error);
                needKey = true;
                lastKeyRequest = 0;
                requestKey();
                break;
            }
            _decodeAccum += (NowUs() - t0) / 1000.0;
            _decodeCount++;
            break;
        }
        case proto::kPong:
        {
            if (len < 16)
                break;
            int64_t now = NowUs();
            int64_t sent = Be64(p), phoneNow = Be64(p + 8);
            int64_t rtt = now - sent;
            rtts.push_back(rtt / 1000.0);
            if (rtts.size() > 5)
                rtts.pop_front();
            std::vector<double> sorted(rtts.begin(), rtts.end());
            std::sort(sorted.begin(), sorted.end());
            // Trust the clock offset from the fastest round trips.
            if (!_clockValid || rtt <= _bestRtt + 1500 || now - _bestRttAt > 10000000)
            {
                _clockOffset = phoneNow - (sent + rtt / 2);
                _clockValid = true;
                _bestRtt = rtt;
                _bestRttAt = now;
            }
            std::lock_guard<std::mutex> lk(_statsLock);
            _stats.rttMs = sorted[sorted.size() / 2];
            _stats.clockOffsetMs = _clockOffset / 1000.0;
            break;
        }
        case proto::kOrient:
        {
            if (len < 2)
                break;
            _rotation = Be16(p);
            std::lock_guard<std::mutex> lk(_statsLock);
            _stats.rotation = _rotation;
            break;
        }
        case proto::kInfo:
            ParseInfo(p, len);
            break;
        }

        int64_t now = NowUs();
        if (now - statsAt >= 1000000)
        {
            double secs = (now - statsAt) / 1e6;
            std::lock_guard<std::mutex> lk(_statsLock);
            _stats.inFps = _frameCounter / secs;
            _stats.inMbps = _byteCounter * 8 / secs / 1e6;
            _stats.decodeMs = _decodeCount ? _decodeAccum / _decodeCount : 0;
            _stats.latencyMs = _latencyAvg;
            _frameCounter = 0;
            _byteCounter = 0;
            _decodeAccum = 0;
            _decodeCount = 0;
            statsAt = now;
        }
    }

    alive = false;
    shutdown(s, SD_BOTH);
    pinger.join();
    SOCKET expected = s;
    _client.compare_exchange_strong(expected, INVALID_SOCKET);
    closesocket(s);
    {
        std::lock_guard<std::mutex> lk(_statsLock);
        _stats.connected = false;
        _stats.inFps = _stats.inMbps = 0;
        _stats.latencyMs = _stats.rttMs = -1;
        _phone = PhoneInfo{};
        _phoneSerial++;
    }
    CoUninitialize();
}

void Server::OnFrame(const uint8_t* y, int yPitch, const uint8_t* uv, int uvPitch, int w, int h, int64_t ptsUs,
                     bool fullRange)
{
    bool rotate = (_rotation == 180) != _flip;
    bool flipV = rotate;
    bool flipH = rotate != _mirror;
    {
        std::lock_guard<std::mutex> lk(g_writerLock);
        _writer.Publish(y, yPitch, uv, uvPitch, w, h, _fps, flipH, flipV, fullRange);
    }

    int64_t now = NowUs();
    if (_clockValid)
    {
        double latency = (now - (ptsUs - _clockOffset)) / 1000.0;
        if (latency > -20 && latency < 5000)
            _latencyAvg = _latencyAvg < 0 ? latency : _latencyAvg * 0.9 + latency * 0.1;
    }
    if (_previewOn && now - _lastPreviewUs >= 66000)
    {
        _lastPreviewUs = now;
        UpdatePreview(y, yPitch, uv, uvPitch, w, h, flipH, flipV, fullRange);
    }
}

static inline uint8_t Clamp8(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

void Server::UpdatePreview(const uint8_t* y, int yPitch, const uint8_t* uv, int uvPitch, int w, int h, bool flipH,
                           bool flipV, bool fullRange)
{
    int pw = std::min(w, 640);
    int ph = std::max(2, (int)((int64_t)h * pw / w));
    std::vector<uint32_t> img((size_t)pw * ph);
    for (int py = 0; py < ph; py++)
    {
        int sy = (int)((int64_t)py * h / ph);
        if (flipV)
            sy = h - 1 - sy;
        const uint8_t* yr = y + (size_t)sy * yPitch;
        const uint8_t* uvr = uv + (size_t)(sy / 2) * uvPitch;
        for (int px = 0; px < pw; px++)
        {
            int sx = (int)((int64_t)px * w / pw);
            if (flipH)
                sx = w - 1 - sx;
            int Y = yr[sx], U = uvr[sx & ~1] - 128, V = uvr[(sx & ~1) + 1] - 128;
            int r, g, b;
            if (fullRange)
            {
                int c = Y * 256;
                r = (c + 403 * V) >> 8;
                g = (c - 48 * U - 120 * V) >> 8;
                b = (c + 475 * U) >> 8;
            }
            else
            {
                int c = (Y - 16) * 298;
                r = (c + 459 * V + 128) >> 8;
                g = (c - 55 * U - 136 * V + 128) >> 8;
                b = (c + 541 * U + 128) >> 8;
            }
            img[(size_t)py * pw + px] = (uint32_t)Clamp8(r) << 16 | (uint32_t)Clamp8(g) << 8 | Clamp8(b);
        }
    }
    std::lock_guard<std::mutex> lk(_previewLock);
    _preview.swap(img);
    _previewW = pw;
    _previewH = ph;
    _previewSerial++;
}

void Server::BeaconLoop()
{
    SOCKET u = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    BOOL yes = TRUE;
    setsockopt(u, SOL_SOCKET, SO_BROADCAST, (const char*)&yes, sizeof(yes));
    std::string message = "PHONECAM1 " + std::to_string(proto::kStreamPort) + " " + ToUtf8(ComputerName());
    std::vector<uint8_t> adapters(64 * 1024);

    while (_running)
    {
        std::vector<uint32_t> targets{ 0xFFFFFFFFu };
        std::vector<std::wstring> addresses;
        ULONG size = (ULONG)adapters.size();
        ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
        ULONG rc = GetAdaptersAddresses(AF_INET, flags, nullptr, (IP_ADAPTER_ADDRESSES*)adapters.data(), &size);
        if (rc == ERROR_BUFFER_OVERFLOW)
        {
            adapters.resize(size);
            rc = GetAdaptersAddresses(AF_INET, flags, nullptr, (IP_ADAPTER_ADDRESSES*)adapters.data(), &size);
        }
        if (rc == NO_ERROR)
        {
            for (auto* a = (IP_ADAPTER_ADDRESSES*)adapters.data(); a; a = a->Next)
            {
                if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
                    continue;
                for (auto* ua = a->FirstUnicastAddress; ua; ua = ua->Next)
                {
                    auto* sin = (sockaddr_in*)ua->Address.lpSockaddr;
                    uint32_t ip = ntohl(sin->sin_addr.s_addr);
                    if ((ip >> 16) == 0xA9FE) // 169.254.x.x
                        continue;
                    uint32_t mask = ua->OnLinkPrefixLength ? 0xFFFFFFFFu << (32 - ua->OnLinkPrefixLength) : 0;
                    targets.push_back(ip | ~mask);
                    wchar_t text[64];
                    InetNtopW(AF_INET, &sin->sin_addr, text, 64);
                    addresses.push_back(std::wstring(text) + L" (" + a->FriendlyName + L")");
                }
            }
        }
        std::sort(targets.begin(), targets.end());
        targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
        for (uint32_t t : targets)
        {
            sockaddr_in to{};
            to.sin_family = AF_INET;
            to.sin_port = htons(proto::kDiscoveryPort);
            to.sin_addr.s_addr = htonl(t);
            sendto(u, message.data(), (int)message.size(), 0, (sockaddr*)&to, sizeof(to));
        }
        {
            std::lock_guard<std::mutex> lk(_statsLock);
            _addresses = addresses;
        }
        for (int i = 0; i < 10 && _running; i++)
        {
            if (i % 5 == 0)
            {
                bool connected = GetStats().connected;
                std::lock_guard<std::mutex> lk(g_writerLock);
                _writer.Heartbeat(connected, _english);
            }
            Sleep(100);
        }
    }
    closesocket(u);
}

// USB mode: "adb reverse" lets the phone reach the PC through 127.0.0.1:47801.
void Server::AdbLoop()
{
    std::wstring adb = ExtractAdb(); // embedded in the exe
    if (adb.empty())
        return;

    while (_running)
    {
        if (_usbOn)
        {
            std::wstring cmd = L"\"" + adb + L"\" reverse tcp:" + std::to_wstring(proto::kStreamPort) + L" tcp:" +
                               std::to_wstring(proto::kStreamPort);
            STARTUPINFOW si{ sizeof(si) };
            si.dwFlags = STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_HIDE;
            PROCESS_INFORMATION pi{};
            if (CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
            {
                if (WaitForSingleObject(pi.hProcess, 8000) == WAIT_TIMEOUT)
                    TerminateProcess(pi.hProcess, 1);
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
            }
        }
        for (int i = 0; i < 30 && _running; i++)
            Sleep(100);
    }
}
