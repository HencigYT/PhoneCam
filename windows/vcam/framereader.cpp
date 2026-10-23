#include "framereader.h"
#include <string.h>
#include <algorithm>

using namespace pcam;

FrameReader::~FrameReader()
{
    if (_view)
        UnmapViewOfFile(_view);
    if (_map)
        CloseHandle(_map);
    if (_event)
        CloseHandle(_event);
}

bool FrameReader::EnsureOpen()
{
    if (_view)
        return true;
    ULONGLONG now = GetTickCount64();
    if (_lastOpenTry && now - _lastOpenTry < 500)
        return false;
    _lastOpenTry = now;

    if (!_event)
        _event = OpenOrCreateEvent(_camera);
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
    _seenFrames = Hdr()->frameCount;
    return true;
}

bool FrameReader::WaitForNewFrame(DWORD timeoutMs)
{
    ULONGLONG deadline = GetTickCount64() + timeoutMs;
    for (;;)
    {
        if (EnsureOpen())
        {
            LONG64 count = Hdr()->frameCount;
            if (count != _seenFrames && GetTickCount64() - Hdr()->writerTick < 1500)
                return true;
        }
        ULONGLONG now = GetTickCount64();
        if (now >= deadline)
            return false;
        DWORD wait = (DWORD)std::min<ULONGLONG>(deadline - now, 20);
        if (_event)
            WaitForSingleObject(_event, wait);
        else
            Sleep(wait);
    }
}

void FrameReader::SkipCurrent()
{
    if (_view)
        _seenFrames = Hdr()->frameCount;
}

uint32_t FrameReader::SourceFps()
{
    if (!EnsureOpen())
        return 0;
    return (uint32_t)Hdr()->fps;
}

bool FrameReader::SourceFormat(uint32_t& width, uint32_t& height, uint32_t& fps)
{
    if (!EnsureOpen() || Hdr()->frameCount == 0)
        return false;
    LONG slot = Hdr()->latestSlot;
    if (slot < 0 || slot >= (LONG)kSlotCount)
        return false;
    width = (uint32_t)Hdr()->slots[slot].width;
    height = (uint32_t)Hdr()->slots[slot].height;
    fps = (uint32_t)Hdr()->fps;
    return width >= 64 && height >= 64;
}

// Dark card with a message, rendered once per size/state with GDI and kept as luma.
void FrameReader::BuildPlaceholder(uint32_t w, uint32_t h, int state)
{
    _placeholder.assign((size_t)w * h, 20);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = (LONG)w;
    bi.bmiHeader.biHeight = -(LONG)h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bmp = dc ? CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
    if (!bmp)
    {
        if (dc)
            DeleteDC(dc);
        return;
    }
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    RECT all{ 0, 0, (LONG)w, (LONG)h };
    HBRUSH bg = CreateSolidBrush(RGB(17, 19, 24));
    FillRect(dc, &all, bg);
    DeleteObject(bg);
    SetBkMode(dc, TRANSPARENT);

    const wchar_t* title = L"PhoneCam";
    // 0: program not running (both languages, the language setting is unknown), 1: Hungarian, 2: English
    const wchar_t* line1 = state == 0   ? L"A PhoneCam program nem fut ezen a gépen"
                           : state == 1 ? L"Várakozás a telefonra…"
                                        : L"Waiting for the phone…";
    const wchar_t* line2 = state == 0   ? L"PhoneCam is not running on this PC"
                           : state == 1 ? L"Indítsd el a PhoneCam appot a telefonon"
                                        : L"Start the PhoneCam app on your phone";
    HFONT big = CreateFontW(-(int)(h / 9), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0,
                            L"Segoe UI");
    HFONT small = CreateFontW(-(int)(h / 26), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY,
                              0, L"Segoe UI");
    RECT r1{ 0, (LONG)(h * 30 / 100), (LONG)w, (LONG)(h * 48 / 100) };
    RECT r2{ 0, (LONG)(h * 52 / 100), (LONG)w, (LONG)(h * 59 / 100) };
    RECT r3{ 0, (LONG)(h * 60 / 100), (LONG)w, (LONG)(h * 67 / 100) };
    HGDIOBJ oldFont = SelectObject(dc, big);
    SetTextColor(dc, RGB(235, 238, 242));
    DrawTextW(dc, title, -1, &r1, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, small);
    SetTextColor(dc, RGB(200, 204, 210));
    DrawTextW(dc, line1, -1, &r2, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SetTextColor(dc, RGB(130, 136, 145));
    DrawTextW(dc, line2, -1, &r3, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    GdiFlush();

    const uint8_t* px = (const uint8_t*)bits;
    for (size_t i = 0; i < (size_t)w * h; i++, px += 4)
        _placeholder[i] = (uint8_t)(16 + ((66 * px[2] + 129 * px[1] + 25 * px[0] + 128) >> 8));

    SelectObject(dc, oldFont);
    SelectObject(dc, oldBmp);
    DeleteObject(big);
    DeleteObject(small);
    DeleteObject(bmp);
    DeleteDC(dc);
}

void FrameReader::FillPlaceholder(uint8_t* dst, LONG pitch, uint32_t w, uint32_t h)
{
    int state = 0;
    if (_view && GetTickCount64() - (ULONGLONG)Hdr()->appTick < 3000)
        state = Hdr()->english ? 2 : 1;
    if (_placeholder.size() != (size_t)w * h || _placeholderState != state)
    {
        BuildPlaceholder(w, h, state);
        _placeholderState = state;
    }
    for (uint32_t y = 0; y < h; y++)
        memcpy(dst + (size_t)y * pitch, _placeholder.data() + (size_t)y * w, w);
    uint8_t* uv = dst + (size_t)pitch * h;
    for (uint32_t y = 0; y < h / 2; y++)
        memset(uv + (size_t)y * pitch, 128, w);
}

static void NV12ToYUY2(const uint8_t* src, uint32_t w, uint32_t h, uint8_t* dst, LONG pitch)
{
    const uint8_t* uvBase = src + (size_t)w * h;
    for (uint32_t y = 0; y < h; y++)
    {
        const uint8_t* yr = src + (size_t)y * w;
        const uint8_t* uv = uvBase + (size_t)(y / 2) * w;
        uint8_t* d = dst + (size_t)y * pitch;
        for (uint32_t x = 0; x < w; x += 2)
        {
            d[0] = yr[x];
            d[1] = uv[x];
            d[2] = yr[x + 1];
            d[3] = uv[x + 1];
            d += 4;
        }
    }
}

void FrameReader::Render(uint8_t* dst, LONG pitch, uint32_t width, uint32_t height, bool yuy2)
{
    if (!yuy2)
    {
        RenderNV12(dst, pitch, width, height);
        return;
    }
    _scratch.resize((size_t)width * height * 3 / 2);
    RenderNV12(_scratch.data(), (LONG)width, width, height);
    NV12ToYUY2(_scratch.data(), width, height, dst, pitch);
}

void FrameReader::RenderNV12(uint8_t* dst, LONG pitch, uint32_t width, uint32_t height)
{
    bool rendered = false;
    if (EnsureOpen())
    {
        Header* hd = Hdr();
        ReaderInfo& me = hd->readers[_camera];
        me.width = (LONG)width;
        me.height = (LONG)height;
        InterlockedExchange64(&me.tick, (LONG64)GetTickCount64());

        LONG64 count = hd->frameCount;
        if (count > 0 && GetTickCount64() - hd->writerTick < 1500)
        {
            for (int attempt = 0; attempt < 2; attempt++)
            {
                LONG slot = hd->latestSlot;
                if (slot < 0 || slot >= (LONG)kSlotCount)
                    break;
                SlotInfo& si = hd->slots[slot];
                LONG64 before = si.seq;
                if (before & 1)
                    continue;
                MemoryBarrier();
                uint32_t sw = (uint32_t)si.width, sh = (uint32_t)si.height;
                if (sw < 2 || sh < 2 || sw > kMaxWidth || sh > kMaxHeight)
                    break;
                ScaleNV12(SlotData(_view, slot), sw, sh, dst, pitch, width, height);
                rendered = true;
                MemoryBarrier();
                if (si.seq == before)
                    break; // not overwritten while copying
            }
            _seenFrames = count;
        }
    }
    if (!rendered)
        FillPlaceholder(dst, pitch, width, height);
}

void FrameReader::ScaleNV12(const uint8_t* src, uint32_t sw, uint32_t sh, uint8_t* dst, LONG pitch,
                            uint32_t dw, uint32_t dh)
{
    uint8_t* dstUV = dst + (size_t)pitch * dh;
    const uint8_t* srcUV = src + (size_t)sw * sh;
    if (sw == dw && sh == dh)
    {
        for (uint32_t y = 0; y < dh; y++)
            memcpy(dst + (size_t)y * pitch, src + (size_t)y * sw, dw);
        for (uint32_t y = 0; y < dh / 2; y++)
            memcpy(dstUV + (size_t)y * pitch, srcUV + (size_t)y * sw, dw);
        return;
    }

    // Center-crop the source to the output aspect ratio, then scale.
    uint32_t cw = sw, ch = sh;
    if ((uint64_t)sw * dh > (uint64_t)dw * sh)
        cw = (uint32_t)((uint64_t)sh * dw / dh) & ~1u;
    else if ((uint64_t)sw * dh < (uint64_t)dw * sh)
        ch = (uint32_t)((uint64_t)sw * dh / dw) & ~1u;
    uint32_t cx = ((sw - cw) / 2) & ~1u, cy = ((sh - ch) / 2) & ~1u;

    ScalePlane(src + (size_t)cy * sw + cx, sw, cw, ch, dst, pitch, dw, dh, 1);
    ScalePlane(srcUV + (size_t)(cy / 2) * sw + cx, sw, cw / 2, ch / 2, dstUV, pitch, dw / 2, dh / 2, 2);
}

// Bilinear scaling of an 8-bit plane; comps = 2 for interleaved UV.
void FrameReader::ScalePlane(const uint8_t* src, size_t srcPitch, uint32_t sw, uint32_t sh,
                             uint8_t* dst, size_t dstPitch, uint32_t dw, uint32_t dh, int comps)
{
    _xIndex.resize(dw * 2);
    _xFrac.resize(dw);
    for (uint32_t x = 0; x < dw; x++)
    {
        double pos = (x + 0.5) * sw / dw - 0.5;
        if (pos < 0)
            pos = 0;
        int i0 = (int)pos;
        int frac = (int)((pos - i0) * 256);
        if (i0 >= (int)sw - 1)
        {
            i0 = (int)sw - 1;
            frac = 0;
        }
        int i1 = std::min(i0 + 1, (int)sw - 1);
        _xIndex[x * 2] = i0 * comps;
        _xIndex[x * 2 + 1] = i1 * comps;
        _xFrac[x] = frac;
    }
    for (uint32_t y = 0; y < dh; y++)
    {
        double pos = (y + 0.5) * sh / dh - 0.5;
        if (pos < 0)
            pos = 0;
        int y0 = (int)pos;
        int fy = (int)((pos - y0) * 256);
        if (y0 >= (int)sh - 1)
        {
            y0 = (int)sh - 1;
            fy = 0;
        }
        int y1 = std::min(y0 + 1, (int)sh - 1);
        const uint8_t* r0 = src + (size_t)y0 * srcPitch;
        const uint8_t* r1 = src + (size_t)y1 * srcPitch;
        uint8_t* d = dst + (size_t)y * dstPitch;
        for (uint32_t x = 0; x < dw; x++)
        {
            int a0 = _xIndex[x * 2], a1 = _xIndex[x * 2 + 1], fx = _xFrac[x];
            for (int c = 0; c < comps; c++)
            {
                int top = r0[a0 + c] * (256 - fx) + r0[a1 + c] * fx;
                int bot = r1[a0 + c] * (256 - fx) + r1[a1 + c] * fx;
                d[x * comps + c] = (uint8_t)((top * (256 - fy) + bot * fy + 32768) >> 16);
            }
        }
    }
}
