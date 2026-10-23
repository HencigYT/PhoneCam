// Shared definitions between PhoneCam.exe (writer) and PhoneCamVCam.dll (reader,
// loaded by the Windows Camera Frame Server service).
#pragma once
#include <windows.h>
#include <sddl.h>
#include <stdint.h>
#include <stdio.h>

namespace pcam {

// Windows lets only one app control a camera at a time, so the same picture is
// offered as several cameras: e.g. Discord uses "PhoneCam", OBS "PhoneCam 2".
constexpr int kCameraCount = 3;

// {7B1E3C52-9A4D-4F6B-8E21-5C3D9F0A6B17} .. 19
static const GUID kCameraClsids[kCameraCount] = {
    { 0x7b1e3c52, 0x9a4d, 0x4f6b, { 0x8e, 0x21, 0x5c, 0x3d, 0x9f, 0x0a, 0x6b, 0x17 } },
    { 0x7b1e3c52, 0x9a4d, 0x4f6b, { 0x8e, 0x21, 0x5c, 0x3d, 0x9f, 0x0a, 0x6b, 0x18 } },
    { 0x7b1e3c52, 0x9a4d, 0x4f6b, { 0x8e, 0x21, 0x5c, 0x3d, 0x9f, 0x0a, 0x6b, 0x19 } },
};
static const wchar_t* const kCameraClsidStrings[kCameraCount] = {
    L"{7B1E3C52-9A4D-4F6B-8E21-5C3D9F0A6B17}",
    L"{7B1E3C52-9A4D-4F6B-8E21-5C3D9F0A6B18}",
    L"{7B1E3C52-9A4D-4F6B-8E21-5C3D9F0A6B19}",
};
static const wchar_t* const kCameraNames[kCameraCount] = { L"PhoneCam", L"PhoneCam 2", L"PhoneCam 3" };

// The frame server runs in session 0 as LOCAL SERVICE, so the objects have to
// live in the Global namespace. Whichever side is allowed to create them does.
// PHONECAM_TEST_LOCAL switches to session-local names for the in-process self test.
inline bool UseLocalNames()
{
    return GetEnvironmentVariableW(L"PHONECAM_TEST_LOCAL", nullptr, 0) > 0;
}

inline const wchar_t* ShmName()
{
    return UseLocalNames() ? L"Local\\PhoneCam_Frames_v2" : L"Global\\PhoneCam_Frames_v2";
}

// One auto-reset event per camera, so every camera wakes up on each new frame.
inline void EventName(int camera, wchar_t* out, size_t size)
{
    swprintf_s(out, size, L"%s\\PhoneCam_NewFrame_v2_%d", UseLocalNames() ? L"Local" : L"Global", camera);
}

constexpr uint32_t kMagic = 0x4D414350; // "PCAM"
constexpr uint32_t kVersion = 2;
constexpr uint32_t kMaxWidth = 3840;
constexpr uint32_t kMaxHeight = 2160;
constexpr uint32_t kSlotCount = 3;
constexpr size_t kSlotBytes = (size_t)kMaxWidth * kMaxHeight * 3 / 2; // NV12
constexpr size_t kHeaderBytes = 4096;
constexpr size_t kTotalBytes = kHeaderBytes + kSlotBytes * kSlotCount;

struct SlotInfo
{
    volatile LONG64 seq; // odd while the writer is filling the slot
    volatile LONG width;
    volatile LONG height;
};

struct ReaderInfo
{
    volatile LONG64 tick; // GetTickCount64() of the last frame handed to a camera client
    volatile LONG width;  // resolution the client asked for
    volatile LONG height;
};

struct Header
{
    volatile LONG magic;
    volatile LONG version;
    volatile LONG64 frameCount;  // frames published so far
    volatile LONG latestSlot;    // slot holding the newest complete frame
    volatile LONG fps;           // frame rate of the phone stream
    volatile LONG64 writerTick;  // GetTickCount64() of the last publish
    volatile LONG64 appTick;     // PhoneCam.exe heartbeat, also without a phone
    volatile LONG phoneConnected;
    SlotInfo slots[kSlotCount];
    ReaderInfo readers[kCameraCount];
    volatile LONG english; // UI language of PhoneCam.exe, for the camera placeholder text
};
static_assert(sizeof(Header) <= kHeaderBytes, "header does not fit");

inline uint8_t* SlotData(void* base, uint32_t slot)
{
    return (uint8_t*)base + kHeaderBytes + kSlotBytes * slot;
}

// Everyone (incl. LOCAL SERVICE, app containers and low integrity) may read/write.
inline bool MakeOpenSecurity(SECURITY_ATTRIBUTES& sa, PSECURITY_DESCRIPTOR& sd)
{
    sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:(A;;GA;;;WD)(A;;GA;;;SY)(A;;GA;;;LS)(A;;GA;;;AC)(A;;GA;;;WR)(A;;GA;;;RC)S:(ML;;NW;;;LW)",
            SDDL_REVISION_1, &sd, nullptr))
        return false;
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = sd;
    sa.bInheritHandle = FALSE;
    return true;
}

inline HANDLE OpenOrCreateMapping()
{
    HANDLE h = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, ShmName());
    if (h)
        return h;
    SECURITY_ATTRIBUTES sa{};
    PSECURITY_DESCRIPTOR sd = nullptr;
    bool haveSd = MakeOpenSecurity(sa, sd);
    h = CreateFileMappingW(INVALID_HANDLE_VALUE, haveSd ? &sa : nullptr, PAGE_READWRITE,
                           (DWORD)((uint64_t)kTotalBytes >> 32), (DWORD)kTotalBytes, ShmName());
    if (sd)
        LocalFree(sd);
    return h;
}

inline HANDLE OpenOrCreateEvent(int camera)
{
    wchar_t name[64];
    EventName(camera, name, 64);
    HANDLE h = OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, name);
    if (h)
        return h;
    SECURITY_ATTRIBUTES sa{};
    PSECURITY_DESCRIPTOR sd = nullptr;
    bool haveSd = MakeOpenSecurity(sa, sd);
    h = CreateEventW(haveSd ? &sa : nullptr, FALSE, FALSE, name);
    if (sd)
        LocalFree(sd);
    return h;
}

inline void InitHeader(Header* h)
{
    if (InterlockedCompareExchange(&h->magic, (LONG)kMagic, 0) == 0)
        h->version = kVersion;
}

} // namespace pcam
