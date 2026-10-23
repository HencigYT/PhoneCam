#include "shmwriter.h"
#include <string.h>

using namespace pcam;

ShmWriter::~ShmWriter()
{
    if (_view)
        UnmapViewOfFile(_view);
    if (_map)
        CloseHandle(_map);
    for (HANDLE e : _events)
        if (e)
            CloseHandle(e);
}

// A non-elevated process may not create Global objects, so until the frame
// server has created them (first time an app opens the camera) this keeps retrying.
bool ShmWriter::EnsureOpen()
{
    ULONGLONG now = GetTickCount64();
    if (_view)
    {
        // A camera's event appears once an app opened that camera.
        if (now - _lastEventTry > 1000)
        {
            _lastEventTry = now;
            for (int i = 0; i < kCameraCount; i++)
                if (!_events[i])
                    _events[i] = OpenOrCreateEvent(i);
        }
        return true;
    }
    if (_lastOpenTry && now - _lastOpenTry < 250)
        return false;
    _lastOpenTry = now;
    _map = OpenOrCreateMapping();
    if (!_map)
        return false;
    _view = MapViewOfFile(_map, FILE_MAP_ALL_ACCESS, 0, 0, kTotalBytes);
    if (!_view)
    {
        CloseHandle(_map);
        _map = nullptr;
        return false;
    }
    InitHeader(Hdr());
    for (int i = 0; i < kCameraCount; i++)
        _events[i] = OpenOrCreateEvent(i);
    _lastEventTry = now;
    return true;
}

bool ShmWriter::ClientActive(int camera, int& width, int& height)
{
    if (!EnsureOpen())
        return false;
    const ReaderInfo& r = Hdr()->readers[camera];
    width = r.width;
    height = r.height;
    return GetTickCount64() - (ULONGLONG)r.tick < 1500;
}

void ShmWriter::Heartbeat(bool phoneConnected, bool english)
{
    if (!EnsureOpen())
        return;
    Hdr()->phoneConnected = phoneConnected ? 1 : 0;
    Hdr()->english = english ? 1 : 0;
    InterlockedExchange64(&Hdr()->appTick, (LONG64)GetTickCount64());
}

static void CopyRow(uint8_t* d, const uint8_t* s, int elems, int elemSize, bool reverse, const uint8_t* lut)
{
    int bytes = elems * elemSize;
    if (!reverse)
    {
        if (!lut)
            memcpy(d, s, bytes);
        else
            for (int i = 0; i < bytes; i++)
                d[i] = lut[s[i]];
        return;
    }
    if (elemSize == 1)
    {
        const uint8_t* p = s + bytes - 1;
        if (lut)
            for (int i = 0; i < bytes; i++)
                d[i] = lut[*p--];
        else
            for (int i = 0; i < bytes; i++)
                d[i] = *p--;
        return;
    }
    const uint8_t* p = s + bytes - 2;
    for (int i = 0; i < elems; i++, p -= 2)
    {
        d[i * 2] = lut ? lut[p[0]] : p[0];
        d[i * 2 + 1] = lut ? lut[p[1]] : p[1];
    }
}

void ShmWriter::Publish(const uint8_t* y, int yPitch, const uint8_t* uv, int uvPitch, int width, int height,
                        int fps, bool flipH, bool flipV, bool fullRange)
{
    if (!EnsureOpen())
        return;
    width &= ~1;
    height &= ~1;
    if (width < 2 || height < 2 || width > (int)kMaxWidth || height > (int)kMaxHeight)
        return;
    if (fullRange && !_lutReady)
    {
        for (int i = 0; i < 256; i++)
        {
            _lutY[i] = (uint8_t)(16 + (i * 219 + 127) / 255);
            _lutC[i] = (uint8_t)(128 + ((i - 128) * 224) / 255);
        }
        _lutReady = true;
    }
    const uint8_t* lutY = fullRange ? _lutY : nullptr;
    const uint8_t* lutC = fullRange ? _lutC : nullptr;

    Header* hd = Hdr();
    LONG slot = hd->frameCount == 0 ? 0 : (hd->latestSlot + 1) % (LONG)kSlotCount;
    SlotInfo& si = hd->slots[slot];
    uint8_t* dst = SlotData(_view, slot);
    uint8_t* dstUV = dst + (size_t)width * height;

    InterlockedIncrement64(&si.seq); // odd: writing
    for (int row = 0; row < height; row++)
    {
        int srcRow = flipV ? height - 1 - row : row;
        CopyRow(dst + (size_t)row * width, y + (size_t)srcRow * yPitch, width, 1, flipH, lutY);
    }
    for (int row = 0; row < height / 2; row++)
    {
        int srcRow = flipV ? height / 2 - 1 - row : row;
        CopyRow(dstUV + (size_t)row * width, uv + (size_t)srcRow * uvPitch, width / 2, 2, flipH, lutC);
    }
    si.width = width;
    si.height = height;
    InterlockedIncrement64(&si.seq); // even: complete

    InterlockedExchange(&hd->latestSlot, slot);
    hd->fps = fps;
    InterlockedExchange64(&hd->writerTick, (LONG64)GetTickCount64());
    InterlockedIncrement64(&hd->frameCount);
    for (HANDLE e : _events)
        if (e)
            SetEvent(e);
}
