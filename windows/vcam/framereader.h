#pragma once
#include "../common/shared.h"
#include <vector>

// Reads the frames PhoneCam.exe publishes into shared memory and renders them
// in the size/format the camera client negotiated.
class FrameReader
{
public:
    explicit FrameReader(int camera = 0) : _camera(camera) {}
    void SetCamera(int camera) { _camera = camera; }
    ~FrameReader();

    // True once a frame newer than the last rendered one is available.
    bool WaitForNewFrame(DWORD timeoutMs);
    // Marks the current newest frame as consumed without rendering it.
    void SkipCurrent();
    void Render(uint8_t* dst, LONG pitch, uint32_t width, uint32_t height, bool yuy2);
    // Size and frame rate of the phone stream, if one has been seen.
    bool SourceFormat(uint32_t& width, uint32_t& height, uint32_t& fps);
    uint32_t SourceFps();

private:
    bool EnsureOpen();
    pcam::Header* Hdr() const { return (pcam::Header*)_view; }
    void RenderNV12(uint8_t* dst, LONG pitch, uint32_t width, uint32_t height);
    void FillPlaceholder(uint8_t* dst, LONG pitch, uint32_t w, uint32_t h);
    void BuildPlaceholder(uint32_t w, uint32_t h, int state);
    void ScaleNV12(const uint8_t* src, uint32_t sw, uint32_t sh, uint8_t* dst, LONG pitch, uint32_t dw, uint32_t dh);
    void ScalePlane(const uint8_t* src, size_t srcPitch, uint32_t sw, uint32_t sh,
                    uint8_t* dst, size_t dstPitch, uint32_t dw, uint32_t dh, int comps);

    int _camera;
    HANDLE _map = nullptr;
    HANDLE _event = nullptr;
    void* _view = nullptr;
    ULONGLONG _lastOpenTry = 0;
    LONG64 _seenFrames = 0;
    std::vector<uint8_t> _scratch;
    std::vector<int> _xIndex, _xFrac;
    std::vector<uint8_t> _placeholder; // luma plane, chroma is neutral
    int _placeholderState = -1;
};
