#include "widgets.h"
#include <algorithm>

using namespace Gdiplus;

namespace ui {

static HINSTANCE g_inst;
static int g_dpi = 96;

float Px(float v) { return v * g_dpi / 96.0f; }

void FillRound(Graphics& g, RectF r, float radius, ARGB fill, ARGB border)
{
    GraphicsPath path;
    float d = std::min(radius * 2, std::min(r.Width, r.Height));
    path.AddArc(r.X, r.Y, d, d, 180, 90);
    path.AddArc(r.GetRight() - d, r.Y, d, d, 270, 90);
    path.AddArc(r.GetRight() - d, r.GetBottom() - d, d, d, 0, 90);
    path.AddArc(r.X, r.GetBottom() - d, d, d, 90, 90);
    path.CloseFigure();
    SolidBrush brush{ Color(fill) };
    g.FillPath(&brush, &path);
    if (border)
    {
        Pen pen(Color(border), 1.0f);
        g.DrawPath(&pen, &path);
    }
}

void Text(Graphics& g, const std::wstring& s, RectF r, float size, ARGB color, bool bold, StringAlignment align,
          StringAlignment lineAlign)
{
    FontFamily family(L"Segoe UI");
    Font font(&family, Px(size), bold ? FontStyleBold : FontStyleRegular, UnitPixel);
    SolidBrush brush{ Color(color) };
    StringFormat fmt;
    fmt.SetAlignment(align);
    fmt.SetLineAlignment(lineAlign);
    fmt.SetTrimming(StringTrimmingEllipsisCharacter);
    fmt.SetFormatFlags(StringFormatFlagsNoWrap);
    g.DrawString(s.c_str(), -1, &font, r, &fmt, &brush);
}

// Paints a child control into a back buffer to avoid flicker.
template <typename F>
static void PaintBuffered(HWND hwnd, F draw)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
    HGDIOBJ old = SelectObject(mem, bmp);
    {
        Graphics g(mem);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
        g.Clear(Color(kCard));
        draw(g, (float)rc.right, (float)rc.bottom);
    }
    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

// ------------------------------------------------------------------ toggle

struct ToggleState
{
    bool on = false;
    bool hover = false;
};

static LRESULT CALLBACK ToggleProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    auto* st = (ToggleState*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg)
    {
    case WM_NCCREATE:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR) new ToggleState());
        break;
    case WM_NCDESTROY:
        delete st;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_ENABLE:
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_MOUSEMOVE:
        if (!st->hover)
        {
            st->hover = true;
            TRACKMOUSEEVENT t{ sizeof(t), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&t);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_MOUSELEAVE:
        st->hover = false;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_LBUTTONUP:
        if (IsWindowEnabled(hwnd))
        {
            st->on = !st->on;
            InvalidateRect(hwnd, nullptr, FALSE);
            SendMessageW(GetParent(hwnd), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(hwnd), BN_CLICKED), (LPARAM)hwnd);
        }
        return 0;
    case WM_PAINT:
    {
        wchar_t label[128];
        GetWindowTextW(hwnd, label, 128);
        bool enabled = IsWindowEnabled(hwnd) != FALSE;
        PaintBuffered(hwnd, [&](Graphics& g, float w, float h) {
            float tw = Px(38), th = Px(20);
            RectF track(w - tw - Px(2), (h - th) / 2, tw, th);
            Text(g, label, RectF(0, 0, w - tw - Px(10), h), 14, enabled ? (st->hover ? 0xFFFFFFFF : kText) : kFaint);
            ARGB trackColor = st->on ? (enabled ? kAccent : 0xFF2C4A7A) : kTrack;
            FillRound(g, track, th / 2, trackColor);
            float knob = th - Px(6);
            float kx = st->on ? track.GetRight() - knob - Px(3) : track.X + Px(3);
            SolidBrush white{ Color(enabled ? 0xFFFFFFFF : 0xFF8A9099) };
            g.FillEllipse(&white, kx, track.Y + Px(3), knob, knob);
        });
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

HWND CreateToggle(HWND parent, int id, const wchar_t* label, int x, int y, int w, int h)
{
    return CreateWindowExW(0, L"PhoneCamToggle", label, WS_CHILD | WS_VISIBLE, x, y, w, h, parent,
                           (HMENU)(INT_PTR)id, g_inst, nullptr);
}

void SetToggle(HWND toggle, bool on)
{
    auto* st = (ToggleState*)GetWindowLongPtrW(toggle, GWLP_USERDATA);
    if (st && st->on != on)
    {
        st->on = on;
        InvalidateRect(toggle, nullptr, FALSE);
    }
}

bool GetToggle(HWND toggle)
{
    auto* st = (ToggleState*)GetWindowLongPtrW(toggle, GWLP_USERDATA);
    return st && st->on;
}

// ------------------------------------------------------------------ slider

struct SliderState
{
    int value = 0;
    bool dragging = false;
};

static void SliderFromMouse(HWND hwnd, SliderState* st, int x)
{
    RECT rc;
    GetClientRect(hwnd, &rc);
    float pad = Px(9);
    float t = (x - pad) / std::max(1.0f, rc.right - 2 * pad);
    int v = (int)(std::min(1.0f, std::max(0.0f, t)) * 1000 + 0.5f);
    if (v != st->value)
    {
        st->value = v;
        InvalidateRect(hwnd, nullptr, FALSE);
        SendMessageW(GetParent(hwnd), WM_SLIDER_MOVED, GetDlgCtrlID(hwnd), v);
    }
}

static LRESULT CALLBACK SliderProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    auto* st = (SliderState*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg)
    {
    case WM_NCCREATE:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR) new SliderState());
        break;
    case WM_NCDESTROY:
        delete st;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_ENABLE:
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_LBUTTONDOWN:
        if (!IsWindowEnabled(hwnd))
            return 0;
        SetCapture(hwnd);
        st->dragging = true;
        SliderFromMouse(hwnd, st, (short)LOWORD(lp));
        return 0;
    case WM_MOUSEMOVE:
        if (st->dragging)
            SliderFromMouse(hwnd, st, (short)LOWORD(lp));
        return 0;
    case WM_LBUTTONUP:
    case WM_CAPTURECHANGED:
        if (st->dragging)
        {
            st->dragging = false;
            if (msg == WM_LBUTTONUP)
                ReleaseCapture();
            SendMessageW(GetParent(hwnd), WM_SLIDER_RELEASED, GetDlgCtrlID(hwnd), st->value);
        }
        return 0;
    case WM_PAINT:
    {
        bool enabled = IsWindowEnabled(hwnd) != FALSE;
        PaintBuffered(hwnd, [&](Graphics& g, float w, float h) {
            float pad = Px(9), th = Px(4);
            float x = pad + (w - 2 * pad) * st->value / 1000.0f;
            FillRound(g, RectF(pad, (h - th) / 2, w - 2 * pad, th), th / 2, kTrack);
            if (enabled)
                FillRound(g, RectF(pad, (h - th) / 2, x - pad, th), th / 2, kAccent);
            float k = Px(16);
            SolidBrush knob{ Color(enabled ? 0xFFFFFFFF : 0xFF6A707A) };
            g.FillEllipse(&knob, x - k / 2, (h - k) / 2, k, k);
        });
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

HWND CreateSlider(HWND parent, int id, int x, int y, int w, int h)
{
    return CreateWindowExW(0, L"PhoneCamSlider", nullptr, WS_CHILD | WS_VISIBLE, x, y, w, h, parent,
                           (HMENU)(INT_PTR)id, g_inst, nullptr);
}

void SetSlider(HWND slider, int value)
{
    auto* st = (SliderState*)GetWindowLongPtrW(slider, GWLP_USERDATA);
    if (st && !st->dragging && st->value != value)
    {
        st->value = value;
        InvalidateRect(slider, nullptr, FALSE);
    }
}

bool SliderDragging(HWND slider)
{
    auto* st = (SliderState*)GetWindowLongPtrW(slider, GWLP_USERDATA);
    return st && st->dragging;
}

void Init(HINSTANCE inst, int dpi)
{
    g_inst = inst;
    g_dpi = dpi;
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_HAND);
    wc.lpfnWndProc = ToggleProc;
    wc.lpszClassName = L"PhoneCamToggle";
    RegisterClassExW(&wc);
    wc.lpfnWndProc = SliderProc;
    wc.lpszClassName = L"PhoneCamSlider";
    RegisterClassExW(&wc);
}

} // namespace ui
