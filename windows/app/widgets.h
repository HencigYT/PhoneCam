#pragma once
#include <windows.h>
#include <objidl.h>
#include <algorithm>
namespace Gdiplus
{
using std::max; // GDI+ headers expect min/max, which NOMINMAX removes
using std::min;
}
#include <gdiplus.h>
#include <string>

// Dark theme palette and the small custom controls of the PhoneCam window.
namespace ui {

constexpr Gdiplus::ARGB kBg = 0xFF111318;
constexpr Gdiplus::ARGB kCard = 0xFF1A1D24;
constexpr Gdiplus::ARGB kTile = 0xFF21252E;
constexpr Gdiplus::ARGB kBorder = 0xFF2A2F39;
constexpr Gdiplus::ARGB kText = 0xFFE8EAED;
constexpr Gdiplus::ARGB kMuted = 0xFF9AA0A6;
constexpr Gdiplus::ARGB kFaint = 0xFF5F6670;
constexpr Gdiplus::ARGB kAccent = 0xFF3B82F6;
constexpr Gdiplus::ARGB kGreen = 0xFF22C55E;
constexpr Gdiplus::ARGB kYellow = 0xFFEAB308;
constexpr Gdiplus::ARGB kRed = 0xFFEF4444;
constexpr Gdiplus::ARGB kTrack = 0xFF3A3F4B;
constexpr COLORREF kCardRgb = RGB(0x1A, 0x1D, 0x24);
constexpr COLORREF kBgRgb = RGB(0x11, 0x13, 0x18);

// Sent to the parent while a slider moves: wParam = control id, lParam = 0..1000.
constexpr UINT WM_SLIDER_MOVED = WM_APP + 1;
constexpr UINT WM_SLIDER_RELEASED = WM_APP + 2;

void Init(HINSTANCE inst, int dpi);
float Px(float v); // DPI scaling

void FillRound(Gdiplus::Graphics& g, Gdiplus::RectF r, float radius, Gdiplus::ARGB fill, Gdiplus::ARGB border = 0);
void Text(Gdiplus::Graphics& g, const std::wstring& s, Gdiplus::RectF r, float size, Gdiplus::ARGB color,
          bool bold = false, Gdiplus::StringAlignment align = Gdiplus::StringAlignmentNear,
          Gdiplus::StringAlignment lineAlign = Gdiplus::StringAlignmentCenter);

// Toggle switch with a label; notifies the parent with WM_COMMAND/BN_CLICKED.
HWND CreateToggle(HWND parent, int id, const wchar_t* label, int x, int y, int w, int h);
void SetToggle(HWND toggle, bool on);
bool GetToggle(HWND toggle);

// Horizontal slider, value 0..1000.
HWND CreateSlider(HWND parent, int id, int x, int y, int w, int h);
void SetSlider(HWND slider, int value);
bool SliderDragging(HWND slider);

} // namespace ui
