#pragma once
#include "../common/shared.h"

// Publishes decoded NV12 frames to the virtual camera DLL through shared memory.
class ShmWriter
{
public:
    ~ShmWriter();

    // flipH/flipV mirror the image; fullRange squeezes 0-255 YUV into video range.
    void Publish(const uint8_t* y, int yPitch, const uint8_t* uv, int uvPitch, int width, int height, int fps,
                 bool flipH, bool flipV, bool fullRange);
    // True while an app (Discord, OBS, ...) pulls frames from that camera; reports its resolution.
    bool ClientActive(int camera, int& width, int& height);
    // Lets the cameras tell "app not running" from "no phone" while showing the placeholder.
    void Heartbeat(bool phoneConnected, bool english);

private:
    bool EnsureOpen();
    pcam::Header* Hdr() const { return (pcam::Header*)_view; }

    HANDLE _map = nullptr;
    HANDLE _events[pcam::kCameraCount] = {};
    ULONGLONG _lastEventTry = 0;
    void* _view = nullptr;
    ULONGLONG _lastOpenTry = 0;
    uint8_t _lutY[256];
    uint8_t _lutC[256];
    bool _lutReady = false;
};
