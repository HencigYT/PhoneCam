#include "server.h"
#include "decoder.h"
#include "install.h"
#include "widgets.h"
#include "lang.h"
#include <mfapi.h>
#include <mfidl.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <cmath>
#include <string>

using namespace Gdiplus;

static Server g_server;
static HINSTANCE g_inst;
static HWND g_main, g_preview;
static HWND g_cbCamera, g_cbResolution, g_cbFps, g_cbCodec, g_cbBitrate;
static HWND g_tgStab, g_tgTorch, g_slZoom;
static HWND g_tgMirror, g_tgFlip, g_tgPreview, g_tgUsb;
static HFONT g_comboFont;
static HBRUSH g_cardBrush;
static int g_dpi = 96;
static int g_camerasFound = -1;
bool lang::g_english = false;

// HU | EN switch in the header.
static const RECT kLangSwitch{ 626, 18, 712, 48 };

static std::vector<uint32_t> g_pixels;
static int g_pixW = 0, g_pixH = 0;
static uint64_t g_pixSerial = 0;

static PhoneInfo g_phone;
static uint64_t g_phoneSerial = ~0ull;
static bool g_populating = false;
static double g_zoom = 1;
static ULONGLONG g_lastZoomSend = 0;

static const int kBitrates[] = { 0, 6, 10, 15, 20, 30, 40, 60, 80 };

enum
{
    IdCamera = 101,
    IdResolution,
    IdFps,
    IdCodec,
    IdBitrate,
    IdStab,
    IdTorch,
    IdZoom,
    IdMirror,
    IdFlip,
    IdPreview,
    IdUsb,
    TimerStatus = 1,
    TimerPreview = 2,
};

// Layout in 96-dpi units.
static const RECT kPreviewCard{ 24, 64, 704, 464 };
static const RECT kTiles{ 24, 476, 704, 552 };
static const RECT kInfoCard{ 24, 564, 704, 688 };
static const RECT kPhoneCard{ 720, 64, 1064, 168 };
static const RECT kSettingsCard{ 720, 180, 1064, 448 };
static const RECT kControlCard{ 720, 460, 1064, 572 };
static const RECT kPcCard{ 720, 584, 1064, 688 };
static const int kClientW = 1088, kClientH = 712;

static int S(int v) { return MulDiv(v, g_dpi, 96); }
static float F(float v) { return v * g_dpi / 96.0f; }
static RectF R(const RECT& r) { return RectF(F((float)r.left), F((float)r.top), F((float)(r.right - r.left)), F((float)(r.bottom - r.top))); }
static RectF R(float x, float y, float w, float h) { return RectF(F(x), F(y), F(w), F(h)); }

static const wchar_t kSettingsKey[] = L"Software\\PhoneCam";

static DWORD LoadSetting(const wchar_t* name, DWORD def)
{
    DWORD v = def, size = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, name, RRF_RT_REG_DWORD, nullptr, &v, &size);
    return v;
}

static void SaveSetting(const wchar_t* name, DWORD v)
{
    RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, name, REG_DWORD, &v, sizeof(v));
}

static std::wstring Fmt(const wchar_t* fmt, ...)
{
    wchar_t buf[512];
    va_list ap;
    va_start(ap, fmt);
    vswprintf_s(buf, fmt, ap);
    va_end(ap);
    return buf;
}

// The cameras are registered by the setup and stay installed; count what Windows sees.
static int CountPhoneCams()
{
    ComPtr<IMFAttributes> attrs;
    MFCreateAttributes(&attrs, 1);
    attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    IMFActivate** devices = nullptr;
    UINT32 count = 0;
    int found = 0;
    if (FAILED(MFEnumDeviceSources(attrs.Get(), &devices, &count)))
        return 0;
    for (UINT32 i = 0; i < count; i++)
    {
        wchar_t* name = nullptr;
        UINT32 len = 0;
        devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &len);
        if (name && wcsncmp(name, L"PhoneCam", 8) == 0)
            found++;
        CoTaskMemFree(name);
        devices[i]->Release();
    }
    CoTaskMemFree(devices);
    return found;
}

// ------------------------------------------------------------------ phone settings

static double ZoomToPos(double z)
{
    if (g_phone.zoomMax <= g_phone.zoomMin)
        return 0;
    return (std::log(z) - std::log(g_phone.zoomMin)) / (std::log(g_phone.zoomMax) - std::log(g_phone.zoomMin)) * 1000;
}

static double PosToZoom(int pos)
{
    double z = std::exp(std::log(g_phone.zoomMin) + (std::log(g_phone.zoomMax) - std::log(g_phone.zoomMin)) * pos / 1000);
    return std::fabs(z - 1) < 0.04 ? 1.0 : z; // snap to the main lens
}

static const PhoneCamera* CurrentCamera()
{
    for (const PhoneCamera& c : g_phone.cameras)
        if (c.id == g_phone.camera)
            return &c;
    return g_phone.cameras.empty() ? nullptr : &g_phone.cameras[0];
}

static void ComboReset(HWND cb)
{
    SendMessageW(cb, CB_RESETCONTENT, 0, 0);
}

static void ComboAdd(HWND cb, const std::wstring& text, LPARAM data, bool select)
{
    int i = (int)SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)text.c_str());
    SendMessageW(cb, CB_SETITEMDATA, i, data);
    if (select)
        SendMessageW(cb, CB_SETCURSEL, i, 0);
}

static LPARAM ComboData(HWND cb)
{
    int i = (int)SendMessageW(cb, CB_GETCURSEL, 0, 0);
    return i < 0 ? -1 : SendMessageW(cb, CB_GETITEMDATA, i, 0);
}

static std::wstring ResolutionName(int w, int h)
{
    std::wstring tag = h == 2160 ? L"4K" : std::to_wstring(h) + L"p";
    return tag + L"  (" + std::to_wstring(w) + L"×" + std::to_wstring(h) + L")";
}

// Fills the phone controls from the latest report of the phone.
static void PopulateFromPhone()
{
    g_populating = true;
    bool on = g_phone.valid;
    for (HWND cb : { g_cbCamera, g_cbResolution, g_cbFps, g_cbCodec, g_cbBitrate })
    {
        ComboReset(cb);
        EnableWindow(cb, on);
    }
    EnableWindow(g_tgStab, on);
    EnableWindow(g_tgTorch, on && g_phone.hasFlash);
    EnableWindow(g_slZoom, on && g_phone.zoomMax > g_phone.zoomMin + 0.01);
    if (on)
    {
        for (size_t i = 0; i < g_phone.cameras.size(); i++)
            ComboAdd(g_cbCamera, g_phone.cameras[i].label, (LPARAM)i, g_phone.cameras[i].id == g_phone.camera);
        if (const PhoneCamera* cam = CurrentCamera())
        {
            for (auto& r : cam->resolutions)
                ComboAdd(g_cbResolution, ResolutionName(r.first, r.second), MAKELPARAM(r.first, r.second),
                         r.first == g_phone.width && r.second == g_phone.height);
            for (int f : cam->fps)
                ComboAdd(g_cbFps, std::to_wstring(f) + L" fps", f, f == g_phone.fps);
        }
        bool hevc = (g_server.Codecs() & 2) != 0;
        ComboAdd(g_cbCodec, lang::tr(L"H.264 – mindenhol megy", L"H.264 – works everywhere"), 0, g_phone.codec == 0);
        ComboAdd(g_cbCodec, hevc ? lang::tr(L"H.265 (HEVC) – jobb kép", L"H.265 (HEVC) – better image") : lang::tr(L"H.265 – nincs dekóder a PC-n", L"H.265 – no decoder on this PC"), 1, g_phone.codec == 1);
        for (int b : kBitrates)
            ComboAdd(g_cbBitrate, b == 0 ? std::wstring(lang::tr(L"Automatikus", L"Automatic")) : std::to_wstring(b) + L" Mbit/s", b, b == g_phone.bitrate);
        ui::SetToggle(g_tgStab, g_phone.stabilization);
        ui::SetToggle(g_tgTorch, g_phone.torch);
        if (!ui::SliderDragging(g_slZoom))
        {
            g_zoom = g_phone.zoom;
            ui::SetSlider(g_slZoom, (int)ZoomToPos(g_zoom));
        }
    }
    g_populating = false;
}

static void SendToPhone(const std::string& settings)
{
    g_server.SendSettings(settings);
}

static void OnPhoneControl(int id)
{
    if (g_populating || !g_phone.valid)
        return;
    switch (id)
    {
    case IdCamera:
    {
        LPARAM i = ComboData(g_cbCamera);
        if (i >= 0 && i < (LPARAM)g_phone.cameras.size())
            SendToPhone("camera=" + g_phone.cameras[i].id + "\n");
        break;
    }
    case IdResolution:
    {
        LPARAM d = ComboData(g_cbResolution);
        if (d != -1)
            SendToPhone("width=" + std::to_string(LOWORD(d)) + "\nheight=" + std::to_string(HIWORD(d)) + "\n");
        break;
    }
    case IdFps:
        SendToPhone("fps=" + std::to_string(ComboData(g_cbFps)) + "\n");
        break;
    case IdCodec:
        SendToPhone("codec=" + std::to_string(ComboData(g_cbCodec)) + "\n");
        break;
    case IdBitrate:
        SendToPhone("bitrate=" + std::to_string(ComboData(g_cbBitrate)) + "\n");
        break;
    case IdStab:
        SendToPhone(std::string("stab=") + (ui::GetToggle(g_tgStab) ? "1" : "0") + "\n");
        break;
    case IdTorch:
        SendToPhone(std::string("torch=") + (ui::GetToggle(g_tgTorch) ? "1" : "0") + "\n");
        break;
    }
}

static void SendZoom(bool force)
{
    ULONGLONG now = GetTickCount64();
    if (!force && now - g_lastZoomSend < 60)
        return;
    g_lastZoomSend = now;
    char buf[32];
    sprintf_s(buf, "zoom=%.2f\n", g_zoom);
    SendToPhone(buf);
}

// ------------------------------------------------------------------ painting

static void DrawCardTitle(Graphics& g, const RECT& card, const wchar_t* title)
{
    ui::Text(g, title, R(card.left + 16.0f, card.top + 10.0f, 300, 24), 15, ui::kText, true);
}

static void DrawBattery(Graphics& g, float x, float y, int percent, bool charging)
{
    ARGB color = percent < 0 ? ui::kMuted : percent <= 15 ? ui::kRed : percent <= 35 ? ui::kYellow : ui::kGreen;
    RectF body = R(x, y, 34, 17);
    Pen outline(Color(ui::kMuted), F(1.5f));
    g.DrawRectangle(&outline, body);
    SolidBrush tip{ Color(ui::kMuted) };
    g.FillRectangle(&tip, R(x + 34, y + 5, 3, 7));
    if (percent > 0)
    {
        SolidBrush fill{ Color(color) };
        g.FillRectangle(&fill, R(x + 3, y + 3, 28.0f * std::min(percent, 100) / 100, 11));
    }
    std::wstring text = percent < 0 ? L"–" : std::to_wstring(percent) + L"%";
    ui::Text(g, text, R(x + 44, y - 6, 80, 30), 20, ui::kText, true);
    if (charging)
        ui::Text(g, lang::tr(L"⚡ töltés", L"⚡ charging"), R(x + 44, y + 22, 100, 18), 12, ui::kYellow);
}

static void DrawTile(Graphics& g, float x, float y, float w, const wchar_t* label, const std::wstring& value,
                     ARGB valueColor = ui::kText)
{
    ui::FillRound(g, R(x, y, w, 76), F(10), ui::kTile);
    ui::Text(g, label, R(x + 14, y + 10, w - 20, 18), 12, ui::kMuted);
    ui::Text(g, value, R(x + 14, y + 32, w - 20, 32), 17, valueColor, true);
}

static void Paint(HDC dc)
{
    RECT rc;
    GetClientRect(g_main, &rc);
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
    HGDIOBJ old = SelectObject(mem, bmp);
    {
        Graphics g(mem);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
        g.Clear(Color(ui::kBg));

        Stats st = g_server.GetStats();

        // Header
        ui::Text(g, L"PhoneCam", R(24, 14, 200, 36), 24, ui::kText, true);
        ui::Text(g, lang::tr(L"a telefonod mint webkamera", L"your phone as a webcam"), R(168, 20, 300, 30), 13, ui::kMuted);
        std::wstring pill = st.connected ? std::wstring(lang::tr(L"Csatlakozva · ", L"Connected · ")) + st.phoneName : std::wstring(lang::tr(L"Várakozás a telefonra", L"Waiting for the phone"));
        RectF pillRect = R(724, 18, 340, 30);
        ui::FillRound(g, pillRect, F(15), ui::kCard, ui::kBorder);
        SolidBrush dot{ Color(st.connected ? ui::kGreen : ui::kYellow) };
        g.FillEllipse(&dot, pillRect.X + F(14), pillRect.Y + F(11), F(8), F(8));
        ui::Text(g, pill, RectF(pillRect.X + F(30), pillRect.Y, pillRect.Width - F(40), pillRect.Height), 13, ui::kText);

        // Language switch
        RectF ls = R(kLangSwitch);
        ui::FillRound(g, ls, F(15), ui::kCard, ui::kBorder);
        RectF active(lang::g_english ? ls.X + ls.Width / 2 : ls.X, ls.Y, ls.Width / 2, ls.Height);
        ui::FillRound(g, RectF(active.X + F(3), active.Y + F(3), active.Width - F(6), active.Height - F(6)), F(12), ui::kAccent);
        ui::Text(g, L"HU", RectF(ls.X, ls.Y, ls.Width / 2, ls.Height), 12, lang::g_english ? ui::kMuted : 0xFFFFFFFF, true,
                 StringAlignmentCenter);
        ui::Text(g, L"EN", RectF(ls.X + ls.Width / 2, ls.Y, ls.Width / 2, ls.Height), 12,
                 lang::g_english ? 0xFFFFFFFF : ui::kMuted, true, StringAlignmentCenter);

        // Cards
        for (const RECT* card : { &kPreviewCard, &kInfoCard, &kPhoneCard, &kSettingsCard, &kControlCard, &kPcCard })
            ui::FillRound(g, R(*card), F(14), ui::kCard, ui::kBorder);

        // Stat tiles
        float tw = (kTiles.right - kTiles.left - 4 * 10) / 5.0f;
        float tx = (float)kTiles.left, ty = (float)kTiles.top;
        bool live = st.connected && st.width;
        DrawTile(g, tx, ty, tw, lang::tr(L"Képkocka/mp", L"Frame rate"), live ? Fmt(L"%.0f / %d", st.inFps, st.fps) : L"–");
        DrawTile(g, tx + (tw + 10), ty, tw, lang::tr(L"Bitráta", L"Bitrate"), live ? Fmt(L"%.1f Mbit/s", st.inMbps) : L"–");
        DrawTile(g, tx + 2 * (tw + 10), ty, tw, lang::tr(L"Késés", L"Latency"),
                 live && st.latencyMs >= 0 ? Fmt(L"~%.0f ms", st.latencyMs) : L"–",
                 st.latencyMs > 150 ? ui::kYellow : ui::kText);
        DrawTile(g, tx + 3 * (tw + 10), ty, tw, lang::tr(L"Hálózat (RTT)", L"Network (RTT)"), st.connected && st.rttMs >= 0 ? Fmt(L"%.0f ms", st.rttMs) : L"–");
        DrawTile(g, tx + 4 * (tw + 10), ty, tw, lang::tr(L"Dekódolás", L"Decoding"),
                 live ? Fmt(L"%.1f ms %s", st.decodeMs, st.hardwareDecode ? L"GPU" : L"CPU") : L"–");

        // Virtual cameras
        const RECT& ic = kInfoCard;
        DrawCardTitle(g, ic, lang::tr(L"Virtuális kamerák", L"Virtual cameras"));
        ui::Text(g, lang::tr(L"Egy kamerát egyszerre egy program használhat (pl. Discord: 1, OBS: 2)", L"One camera can be used by one program at a time (e.g. Discord: 1, OBS: 2)"),
                 R(ic.left + 170.0f, ic.top + 10.0f, 500, 24), 12, ui::kMuted);
        for (int i = 0; i < pcam::kCameraCount; i++)
        {
            float y = ic.top + 36.0f + i * 20;
            int cw = 0, ch = 0;
            bool used = g_server.ClientActive(i, cw, ch);
            SolidBrush d{ Color(used ? ui::kGreen : ui::kFaint) };
            g.FillEllipse(&d, F(ic.left + 18.0f), F(y + 6), F(8), F(8));
            ui::Text(g, pcam::kCameraNames[i], R(ic.left + 34.0f, y, 140, 20), 13, ui::kText, true);
            ui::Text(g, used ? Fmt(lang::tr(L"használatban · %d×%d", L"in use · %d×%d"), cw, ch) : std::wstring(lang::tr(L"szabad", L"free")), R(ic.left + 150.0f, y, 300, 20), 13,
                     used ? ui::kGreen : ui::kMuted);
        }
        std::wstring footer;
        if (g_camerasFound == 0)
            footer = lang::tr(L"A virtuális kamerák nincsenek telepítve – indítsd újra a PhoneCam-et, és engedélyezd a telepítést.", L"The virtual cameras are not installed – restart PhoneCam and allow the setup.");
        else if (!st.error.empty())
            footer = st.error;
        else
        {
            std::wstring addrs;
            for (auto& a : g_server.LocalAddresses())
                addrs += (addrs.empty() ? L"" : L",  ") + a;
            footer = std::wstring(lang::tr(L"PC címe (kézi megadáshoz):  ", L"PC address (for manual entry):  ")) + (addrs.empty() ? std::wstring(L"–") : addrs);
        }
        ui::Text(g, footer, R(ic.left + 16.0f, ic.top + 98.0f, 650, 20), 12,
                 g_camerasFound == 0 || !st.error.empty() ? ui::kRed : ui::kMuted);

        // Phone
        const RECT& pc = kPhoneCard;
        if (st.connected)
        {
            ui::Text(g, st.phoneName, R(pc.left + 16.0f, pc.top + 14.0f, 190, 26), 17, ui::kText, true);
            std::wstring link = st.usb ? std::wstring(lang::tr(L"USB kábel", L"USB cable")) : L"Wi-Fi · " + st.phoneAddress;
            ui::Text(g, link, R(pc.left + 16.0f, pc.top + 42.0f, 190, 20), 13, ui::kMuted);
            if (st.width)
                ui::Text(g, Fmt(L"%d×%d · %d fps · %s", st.width, st.height, st.fps, st.codec == CodecHEVC ? L"H.265" : L"H.264"),
                         R(pc.left + 16.0f, pc.top + 66.0f, 200, 20), 13, ui::kMuted);
            DrawBattery(g, pc.right - 124.0f, pc.top + 28.0f, g_phone.valid ? g_phone.battery : -1,
                        g_phone.valid && g_phone.charging);
        }
        else
        {
            ui::Text(g, lang::tr(L"Nincs telefon", L"No phone"), R(pc.left + 16.0f, pc.top + 16.0f, 300, 26), 17, ui::kText, true);
            ui::Text(g, lang::tr(L"Indítsd el a PhoneCam appot a telefonon,", L"Start the PhoneCam app on your phone,"), R(pc.left + 16.0f, pc.top + 46.0f, 310, 20), 13, ui::kMuted);
            ui::Text(g, lang::tr(L"ugyanazon a Wi-Fi-n vagy USB kábellel.", L"on the same Wi-Fi or with a USB cable."), R(pc.left + 16.0f, pc.top + 66.0f, 310, 20), 13, ui::kMuted);
        }

        // Phone camera settings
        const RECT& sc = kSettingsCard;
        DrawCardTitle(g, sc, lang::tr(L"Kamera beállítások", L"Camera settings"));
        const wchar_t* rows[] = { lang::tr(L"Kamera", L"Camera"), lang::tr(L"Felbontás", L"Resolution"), lang::tr(L"Képkocka/mp", L"Frame rate"), lang::tr(L"Kodek", L"Codec"), lang::tr(L"Bitráta", L"Bitrate") };
        for (int i = 0; i < 5; i++)
            ui::Text(g, rows[i], R(sc.left + 16.0f, sc.top + 46.0f + i * 36, 120, 30), 13,
                     g_phone.valid ? ui::kMuted : ui::kFaint);

        // Controls
        const RECT& cc = kControlCard;
        DrawCardTitle(g, cc, lang::tr(L"Vezérlés", L"Controls"));
        ui::Text(g, L"Zoom", R(cc.left + 16.0f, cc.top + 74.0f, 60, 28), 14, g_phone.valid ? ui::kText : ui::kFaint);
        ui::Text(g, g_phone.valid ? Fmt(L"%.1f×", g_zoom) : L"–", R(cc.right - 70.0f, cc.top + 74.0f, 54, 28), 14,
                 ui::kText, true, StringAlignmentFar);

        // PC side image options
        DrawCardTitle(g, kPcCard, lang::tr(L"Kép a gépen", L"Image on this PC"));
    }
    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
}

// Texts of the child controls; the painted texts follow lang::g_english by themselves.
static void ApplyLanguage()
{
    SetWindowTextW(g_tgStab, lang::tr(L"Képstabilizálás (több késés)", L"Stabilization (adds delay)"));
    SetWindowTextW(g_tgTorch, lang::tr(L"Vaku (zseblámpa)", L"Flash (torch)"));
    SetWindowTextW(g_tgMirror, lang::tr(L"Tükrözés", L"Mirror"));
    SetWindowTextW(g_tgFlip, lang::tr(L"Forgatás 180°", L"Rotate 180°"));
    SetWindowTextW(g_tgPreview, lang::tr(L"Előnézet", L"Preview"));
    for (HWND t : { g_tgStab, g_tgTorch, g_tgMirror, g_tgFlip, g_tgPreview, g_tgUsb })
        InvalidateRect(t, nullptr, FALSE);
    PopulateFromPhone();
    g_server.SetEnglish(lang::g_english);
    InvalidateRect(g_main, nullptr, FALSE);
    InvalidateRect(g_preview, nullptr, FALSE);
}

static LRESULT CALLBACK PreviewProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        int w = rc.right, h = rc.bottom;
        HBRUSH bg = CreateSolidBrush(RGB(12, 14, 18));
        if (g_pixels.empty())
        {
            HDC mem = CreateCompatibleDC(dc);
            HBITMAP bmp = CreateCompatibleBitmap(dc, w, h);
            HGDIOBJ old = SelectObject(mem, bmp);
            {
                Graphics g(mem);
                g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
                g.Clear(Color(0xFF0C0E12));
                ui::Text(g, lang::tr(L"Nincs kép", L"No image"), RectF(0, h / 2.0f - F(26), (float)w, F(30)), 18, ui::kMuted, true,
                         StringAlignmentCenter);
                ui::Text(g, lang::tr(L"Az előnézet itt jelenik meg, amint a telefon küldi a képet", L"The preview appears here as soon as the phone sends video"),
                         RectF(0, h / 2.0f + F(4), (float)w, F(24)), 13, ui::kFaint, false, StringAlignmentCenter);
            }
            BitBlt(dc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
            SelectObject(mem, old);
            DeleteObject(bmp);
            DeleteDC(mem);
        }
        else
        {
            // Letterbox the preview into the control.
            int dw = w, dh = (int)((int64_t)w * g_pixH / g_pixW);
            if (dh > h)
            {
                dh = h;
                dw = (int)((int64_t)h * g_pixW / g_pixH);
            }
            int dx = (w - dw) / 2, dy = (h - dh) / 2;
            RECT bars[] = { { 0, 0, w, dy }, { 0, dy + dh, w, h }, { 0, dy, dx, dy + dh }, { dx + dw, dy, w, dy + dh } };
            for (RECT& b : bars)
                if (b.right > b.left && b.bottom > b.top)
                    FillRect(dc, &b, bg);
            BITMAPINFO bi{};
            bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
            bi.bmiHeader.biWidth = g_pixW;
            bi.bmiHeader.biHeight = -g_pixH;
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            SetStretchBltMode(dc, HALFTONE);
            SetBrushOrgEx(dc, 0, 0, nullptr);
            StretchDIBits(dc, dx, dy, dw, dh, 0, 0, g_pixW, g_pixH, g_pixels.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
        }
        DeleteObject(bg);
        EndPaint(hwnd, &ps);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static HWND MakeCombo(HWND parent, int id, int row)
{
    HWND cb = CreateWindowExW(0, L"COMBOBOX", nullptr,
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST,
                              S(kSettingsCard.left + 130), S(kSettingsCard.top + 48 + row * 36),
                              S(kSettingsCard.right - kSettingsCard.left - 146), S(320), parent,
                              (HMENU)(INT_PTR)id, g_inst, nullptr);
    SetWindowTheme(cb, L"DarkMode_CFD", nullptr);
    SendMessageW(cb, WM_SETFONT, (WPARAM)g_comboFont, TRUE);
    EnableWindow(cb, FALSE);
    return cb;
}

static LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_CREATE:
    {
        g_main = hwnd;
        BOOL dark = TRUE;
        DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
        COLORREF caption = ui::kBgRgb;
        DwmSetWindowAttribute(hwnd, 35 /*DWMWA_CAPTION_COLOR*/, &caption, sizeof(caption));

        g_preview = CreateWindowExW(0, L"PhoneCamPreview", nullptr, WS_CHILD | WS_VISIBLE, S(kPreviewCard.left + 12),
                                    S(kPreviewCard.top + 12), S(656), S(376), hwnd, nullptr, g_inst, nullptr);

        g_cbCamera = MakeCombo(hwnd, IdCamera, 0);
        g_cbResolution = MakeCombo(hwnd, IdResolution, 1);
        g_cbFps = MakeCombo(hwnd, IdFps, 2);
        g_cbCodec = MakeCombo(hwnd, IdCodec, 3);
        g_cbBitrate = MakeCombo(hwnd, IdBitrate, 4);
        const RECT& sc = kSettingsCard;
        g_tgStab = ui::CreateToggle(hwnd, IdStab, L"Képstabilizálás (több késés)", S(sc.left + 16), S(sc.top + 228),
                                    S(sc.right - sc.left - 32), S(28));

        const RECT& cc = kControlCard;
        g_tgTorch = ui::CreateToggle(hwnd, IdTorch, L"Vaku (zseblámpa)", S(cc.left + 16), S(cc.top + 42),
                                     S(cc.right - cc.left - 32), S(28));
        g_slZoom = ui::CreateSlider(hwnd, IdZoom, S(cc.left + 70), S(cc.top + 76), S(cc.right - cc.left - 150), S(24));

        const RECT& pc = kPcCard;
        int half = (pc.right - pc.left - 48) / 2;
        g_tgMirror = ui::CreateToggle(hwnd, IdMirror, L"Tükrözés", S(pc.left + 16), S(pc.top + 40), S(half), S(26));
        g_tgFlip = ui::CreateToggle(hwnd, IdFlip, L"Forgatás 180°", S(pc.left + 32 + half), S(pc.top + 40), S(half), S(26));
        g_tgPreview = ui::CreateToggle(hwnd, IdPreview, L"Előnézet", S(pc.left + 16), S(pc.top + 70), S(half), S(26));
        g_tgUsb = ui::CreateToggle(hwnd, IdUsb, L"USB (adb)", S(pc.left + 32 + half), S(pc.top + 70), S(half), S(26));
        for (HWND t : { g_tgStab, g_tgTorch, g_slZoom })
            EnableWindow(t, FALSE);

        bool mirror = LoadSetting(L"Mirror", 0) != 0, flip = LoadSetting(L"Flip", 0) != 0;
        bool preview = LoadSetting(L"Preview", 1) != 0, usb = LoadSetting(L"Usb", 1) != 0;
        ui::SetToggle(g_tgMirror, mirror);
        ui::SetToggle(g_tgFlip, flip);
        ui::SetToggle(g_tgPreview, preview);
        ui::SetToggle(g_tgUsb, usb);
        g_server.SetMirror(mirror);
        g_server.SetFlip(flip);
        g_server.SetPreview(preview);
        g_server.SetUsb(usb);

        ApplyLanguage();
        SetTimer(hwnd, TimerStatus, 250, nullptr);
        SetTimer(hwnd, TimerPreview, 33, nullptr);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        Paint(dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN:
    {
        POINT pt{ (short)LOWORD(lp), (short)HIWORD(lp) };
        RECT ls{ S(kLangSwitch.left), S(kLangSwitch.top), S(kLangSwitch.right), S(kLangSwitch.bottom) };
        if (PtInRect(&ls, pt))
        {
            bool english = pt.x >= (ls.left + ls.right) / 2;
            if (english != lang::g_english)
            {
                lang::g_english = english;
                SaveSetting(L"English", english);
                ApplyLanguage();
            }
        }
        return 0;
    }
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC:
    {
        HDC dc = (HDC)wp;
        SetBkColor(dc, ui::kCardRgb);
        SetTextColor(dc, RGB(232, 234, 237));
        return (LRESULT)g_cardBrush;
    }
    case WM_COMMAND:
    {
        int id = LOWORD(wp);
        if (HIWORD(wp) == CBN_SELCHANGE)
        {
            OnPhoneControl(id);
            return 0;
        }
        if (HIWORD(wp) != BN_CLICKED)
            break;
        bool on = ui::GetToggle((HWND)lp);
        switch (id)
        {
        case IdStab:
        case IdTorch:
            OnPhoneControl(id);
            break;
        case IdMirror:
            g_server.SetMirror(on);
            SaveSetting(L"Mirror", on);
            break;
        case IdFlip:
            g_server.SetFlip(on);
            SaveSetting(L"Flip", on);
            break;
        case IdPreview:
            g_server.SetPreview(on);
            SaveSetting(L"Preview", on);
            if (!on)
            {
                g_pixels.clear();
                InvalidateRect(g_preview, nullptr, FALSE);
            }
            break;
        case IdUsb:
            g_server.SetUsb(on);
            SaveSetting(L"Usb", on);
            break;
        }
        return 0;
    }
    case ui::WM_SLIDER_MOVED:
    case ui::WM_SLIDER_RELEASED:
        if (wp == IdZoom && g_phone.valid)
        {
            g_zoom = PosToZoom((int)lp);
            SendZoom(msg == ui::WM_SLIDER_RELEASED);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_TIMER:
        if (wp == TimerStatus)
        {
            uint64_t serial;
            PhoneInfo info = g_server.GetPhoneInfo(serial);
            if (serial != g_phoneSerial)
            {
                g_phoneSerial = serial;
                g_phone = info;
                PopulateFromPhone();
            }
            if (!g_server.GetStats().connected && !g_pixels.empty())
            {
                g_pixels.clear();
                InvalidateRect(g_preview, nullptr, FALSE);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        else if (wp == TimerPreview && ui::GetToggle(g_tgPreview))
        {
            if (g_server.GetPreview(g_pixels, g_pixW, g_pixH, g_pixSerial) && !g_pixels.empty())
                InvalidateRect(g_preview, nullptr, FALSE);
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdLine, int show)
{
    if (wcsstr(cmdLine, L"--install"))
        return DoInstall();
    if (wcsstr(cmdLine, L"--uninstall"))
        return DoUninstall();

    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\PhoneCamApp");
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        HWND existing = FindWindowW(L"PhoneCamMain", nullptr);
        if (existing)
        {
            ShowWindow(existing, SW_RESTORE);
            SetForegroundWindow(existing);
        }
        return 0;
    }

    g_inst = inst;
    g_dpi = GetDpiForSystem();
    lang::g_english = LoadSetting(L"English", PRIMARYLANGID(GetUserDefaultUILanguage()) != LANG_HUNGARIAN) != 0;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);
    GdiplusStartupInput gdiplusInput;
    ULONG_PTR gdiplusToken = 0;
    GdiplusStartup(&gdiplusToken, &gdiplusInput, nullptr);
    ui::Init(inst, g_dpi);

    if (!wcsstr(cmdLine, L"--no-setup") && InstallNeeded())
    {
        unsigned long code = 1;
        if (!RunElevated(L"--install", code) || code != 0)
            MessageBoxW(nullptr,
                        lang::tr(L"A virtuális kamerák telepítéséhez egyszer rendszergazdai jóváhagyás kell.\nIndítsd újra a PhoneCam-et, és a felugró ablakban kattints az Igen-re.",
                                 L"Installing the virtual cameras needs administrator approval once.\nRestart PhoneCam and click Yes in the prompt."),
                        L"PhoneCam", MB_ICONWARNING);
    }
    g_camerasFound = CountPhoneCams();

    std::wstring error;
    if (!g_server.Start(error))
        MessageBoxW(nullptr, error.c_str(), L"PhoneCam", MB_ICONERROR);

    g_comboFont =
        CreateFontW(-S(14), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_cardBrush = CreateSolidBrush(ui::kCardRgb);

    HICON icon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    WNDCLASSEXW pc{ sizeof(pc) };
    pc.lpfnWndProc = PreviewProc;
    pc.hInstance = inst;
    pc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    pc.lpszClassName = L"PhoneCamPreview";
    RegisterClassExW(&pc);

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = MainProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"PhoneCamMain";
    wc.hIcon = icon;
    wc.hIconSm = icon;
    RegisterClassExW(&wc);

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    RECT rc{ 0, 0, S(kClientW), S(kClientH) };
    AdjustWindowRectEx(&rc, style, FALSE, 0);
    CreateWindowExW(0, L"PhoneCamMain", L"PhoneCam", style, CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left,
                    rc.bottom - rc.top, nullptr, nullptr, inst, nullptr);
    ShowWindow(g_main, show);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        if (!IsDialogMessageW(g_main, &msg))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    g_server.Stop();
    GdiplusShutdown(gdiplusToken);
    MFShutdown();
    CoUninitialize();
    CloseHandle(single);
    return 0;
}
