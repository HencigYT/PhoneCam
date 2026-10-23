#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "shmwriter.h"

// Wire protocol (big endian). The phone connects to the PC over TCP.
namespace proto {
constexpr uint16_t kDiscoveryPort = 47800; // UDP beacons PC -> phone
constexpr uint16_t kStreamPort = 47801;    // TCP stream phone -> PC
constexpr uint16_t kTestStreamPort = 47901; // self test, away from real phones
constexpr uint32_t kMagic = 0x5043414D;    // "PCAM"
constexpr uint16_t kVersion = 1;
// phone -> PC
constexpr uint8_t kConfig = 1; // codec u8, width u32, height u32, fps u32, bitrate u32, rotation u16, realtimeClock u8
constexpr uint8_t kFrame = 2;  // flags u8 (1 = key), ptsUs i64, data
constexpr uint8_t kPong = 3;   // pcTimeUs i64, phoneTimeUs i64
constexpr uint8_t kOrient = 4; // rotation u16
constexpr uint8_t kInfo = 5;   // UTF-8 "key=value" lines: battery, settings, camera list (see PhoneInfo)
// PC -> phone
constexpr uint8_t kRequestKey = 1;
constexpr uint8_t kPing = 2; // pcTimeUs i64
constexpr uint8_t kCaps = 3; // codec mask u8, pc name utf8
constexpr uint8_t kSet = 4;  // UTF-8 "key=value" lines: camera, width, height, fps, codec, bitrate, stab, torch, zoom
} // namespace proto

struct Stats
{
    bool connected = false;
    bool usb = false;
    std::wstring phoneName;
    std::wstring phoneAddress;
    int codec = 0, width = 0, height = 0, fps = 0, bitrate = 0, rotation = 0;
    double inFps = 0, inMbps = 0, decodeMs = 0, latencyMs = -1, rttMs = -1;
    bool hardwareDecode = false;
    int decodeErrors = 0;
    double clockOffsetMs = 0;
    std::wstring error;
};

// What the phone reports about itself, so the PC can show and change its settings.
struct PhoneCamera
{
    std::string id;
    std::wstring label;
    std::vector<std::pair<int, int>> resolutions;
    std::vector<int> fps;
};

struct PhoneInfo
{
    bool valid = false;
    int battery = -1;
    bool charging = false;
    std::string camera;
    int width = 0, height = 0, fps = 0, codec = 0, bitrate = 0;
    bool stabilization = false, torch = false, hasFlash = false;
    double zoom = 1, zoomMin = 1, zoomMax = 1;
    std::vector<PhoneCamera> cameras;
};

class Server
{
public:
    bool Start(std::wstring& error);
    void Stop();

    Stats GetStats();
    std::vector<std::wstring> LocalAddresses();
    int Codecs() const { return _codecs; }
    bool ClientActive(int camera, int& width, int& height);
    // Latest phone info; serial changes whenever a new report arrives.
    PhoneInfo GetPhoneInfo(uint64_t& serial);
    // Sends "key=value" settings to the phone. False if no phone is connected.
    bool SendSettings(const std::string& settings);

    void SetMirror(bool on) { _mirror = on; }
    void SetFlip(bool on) { _flip = on; }
    void SetPreview(bool on) { _previewOn = on; }
    void SetUsb(bool on) { _usbOn = on; }
    void SetEnglish(bool on) { _english = on; }
    // Copies the latest preview image (BGRA) if it changed since `serial`.
    bool GetPreview(std::vector<uint32_t>& pixels, int& width, int& height, uint64_t& serial);

private:
    void AcceptLoop();
    void ClientLoop(SOCKET s, std::wstring address);
    void BeaconLoop();
    void AdbLoop();
    bool Send(SOCKET s, uint8_t type, const void* payload, uint32_t size);
    void OnFrame(const uint8_t* y, int yPitch, const uint8_t* uv, int uvPitch, int w, int h, int64_t ptsUs,
                 bool fullRange);
    void ParseInfo(const uint8_t* data, size_t size);
    void UpdatePreview(const uint8_t* y, int yPitch, const uint8_t* uv, int uvPitch, int w, int h, bool flipH,
                       bool flipV, bool fullRange);

    std::atomic<bool> _running{ false };
    SOCKET _listen = INVALID_SOCKET;
    std::atomic<SOCKET> _client{ INVALID_SOCKET };
    std::thread _acceptThread, _clientThread, _beaconThread, _adbThread;
    std::mutex _sendLock;
    int _codecs = 1;

    std::mutex _statsLock;
    Stats _stats;
    std::vector<std::wstring> _addresses;
    PhoneInfo _phone;
    uint64_t _phoneSerial = 0;

    std::atomic<bool> _mirror{ false }, _flip{ false }, _previewOn{ true }, _usbOn{ true }, _english{ false };
    std::atomic<int> _rotation{ 0 }, _fps{ 0 };

    // clock sync and latency
    std::atomic<int64_t> _clockOffset{ 0 }; // phone clock - PC clock (us)
    std::atomic<bool> _clockValid{ false };
    int64_t _bestRtt = 0, _bestRttAt = 0;
    double _latencyAvg = -1;

    // rate counters, reset every second by the client loop
    int _frameCounter = 0;
    int64_t _byteCounter = 0;
    double _decodeAccum = 0;
    int _decodeCount = 0;

    ShmWriter _writer;

    std::mutex _previewLock;
    std::vector<uint32_t> _preview;
    int _previewW = 0, _previewH = 0;
    uint64_t _previewSerial = 0;
    int64_t _lastPreviewUs = 0;
};

int64_t NowUs();
