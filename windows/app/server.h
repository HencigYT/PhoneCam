#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <atomic>
#include <memory>
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

// Accepts any number of phones (Wi-Fi and USB) and feeds each virtual camera
// from the phone chosen for it. Phones are identified by their name, which is
// made unique ("Name (2)") when two connected phones share one.
class Server
{
public:
    Server();
    bool Start(std::wstring& error);
    void Stop();

    // Connected phones in connection order.
    std::vector<std::wstring> Phones();
    // Changes whenever a phone connects, leaves or sends a report.
    uint64_t PhonesSerial();
    Stats GetStats(const std::wstring& phone);
    PhoneInfo GetPhoneInfo(const std::wstring& phone);
    // Sends "key=value" settings to the phone. False if it is not connected.
    bool SendSettings(const std::wstring& phone, const std::string& settings);

    // The phone a virtual camera shows: a phone name, or "" for the first connected phone.
    void SetCameraPhone(int camera, const std::wstring& phone);
    // The connected phone the camera shows right now ("" when none).
    std::wstring CameraSource(int camera);
    bool ClientActive(int camera, int& width, int& height);

    std::vector<std::wstring> LocalAddresses();
    int Codecs() const { return _codecs; }
    void SetMirror(bool on) { _mirror = on; }
    void SetFlip(bool on) { _flip = on; }
    void SetPreview(bool on) { _previewOn = on; }
    void SetUsb(bool on) { _usbOn = on; }
    void SetEnglish(bool on) { _english = on; }
    // The phone shown in the preview ("" = the first one).
    void SetPreviewPhone(const std::wstring& phone);
    // Copies the latest preview image (BGRA) if it changed since `serial`.
    bool GetPreview(std::vector<uint32_t>& pixels, int& width, int& height, uint64_t& serial);

private:
    struct Session;
    void AcceptLoop();
    void ClientLoop(std::shared_ptr<Session> session, std::wstring address);
    void BeaconLoop();
    void AdbLoop();
    bool Send(Session& s, uint8_t type, const void* payload, uint32_t size);
    std::shared_ptr<Session> Resolve(int camera); // needs _lock
    void OnFrame(Session& s, const uint8_t* y, int yPitch, const uint8_t* uv, int uvPitch, int w, int h,
                 int64_t ptsUs, bool fullRange);
    void UpdatePreview(const uint8_t* y, int yPitch, const uint8_t* uv, int uvPitch, int w, int h, bool flipH,
                       bool flipV, bool fullRange);

    std::atomic<bool> _running{ false };
    SOCKET _listen = INVALID_SOCKET;
    std::thread _acceptThread, _beaconThread, _adbThread;
    std::vector<std::shared_ptr<Session>> _clients; // every client thread, owned by the accept thread
    int _codecs = 1;

    std::mutex _lock; // the fields below
    std::vector<std::shared_ptr<Session>> _sessions; // connected phones, in connection order
    uint64_t _serial = 0;
    std::wstring _cameraPhone[pcam::kMaxCameras];
    std::wstring _previewPhone;
    std::vector<std::wstring> _addresses;

    std::atomic<bool> _mirror{ false }, _flip{ false }, _previewOn{ true }, _usbOn{ true }, _english{ false };

    ShmWriter _writers[pcam::kMaxCameras];
    std::mutex _writerLocks[pcam::kMaxCameras];

    std::mutex _previewLock;
    std::vector<uint32_t> _preview;
    int _previewW = 0, _previewH = 0;
    uint64_t _previewSerial = 0;
};

int64_t NowUs();
