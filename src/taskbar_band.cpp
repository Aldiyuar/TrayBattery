#include "taskbar_band.h"
#include <objidl.h>
#include <gdiplus.h>
#include <d2d1.h>
#include <dwrite.h>
#include <commctrl.h>
#include <shellapi.h>

using namespace Gdiplus;
using namespace Gdiplus::DllExports;

static HINSTANCE g_hInst = nullptr;
static HWND g_mainWnd = nullptr;
static HWND g_bandWnd = nullptr;
static HWND g_tipWnd = nullptr;
static ULONG_PTR g_gdipToken = 0;

static ID2D1Factory* g_d2dFactory = nullptr;
static ID2D1DCRenderTarget* g_d2dRT = nullptr;
static IDWriteFactory* g_dwFactory = nullptr;

static DeviceInfo g_bandDevs[MAX_TRAY_ICONS] = {};
static int g_bandCount = 0;
static RECT g_itemRects[MAX_TRAY_ICONS] = {};
static int g_hoverIdx = -1;
static bool g_trackingMouse = false;
static bool g_lightTheme = false;

static int g_bandWidth = 0;
static int g_bandHeight = 40;
static HWND g_ownerTray = nullptr;
static HWND g_lastRebarHwnd = nullptr;
static int g_lastRebarShrink = 0;
static int g_lastPosX = -1;
static int g_lastPosY = -1;
static int g_lastPosW = -1;
static int g_lastPosH = -1;
static bool g_bandVisible = false;

static constexpr UINT_PTR BAND_SYNC_TIMER_ID = 77;
static constexpr UINT BAND_SYNC_MS = 400;

static void EnsureDirectWrite() {
    if (!g_d2dFactory) {
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), nullptr,
                          reinterpret_cast<void**>(&g_d2dFactory));
    }
    if (g_d2dFactory && !g_d2dRT) {
        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0f, 96.0f,
            D2D1_RENDER_TARGET_USAGE_NONE,
            D2D1_FEATURE_LEVEL_DEFAULT);
        g_d2dFactory->CreateDCRenderTarget(&props, &g_d2dRT);
        if (g_d2dRT) {
            g_d2dRT->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        }
    }
    if (!g_dwFactory) {
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                            reinterpret_cast<IUnknown**>(&g_dwFactory));
    }
}

static void ReleaseDirectWrite() {
    if (g_d2dRT) { g_d2dRT->Release(); g_d2dRT = nullptr; }
    if (g_d2dFactory) { g_d2dFactory->Release(); g_d2dFactory = nullptr; }
    if (g_dwFactory) { g_dwFactory->Release(); g_dwFactory = nullptr; }
}

static bool IsTaskbarLightTheme() {
    HKEY hKey = nullptr;
    DWORD val = 0;
    DWORD sz = sizeof(val);
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                      0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        RegQueryValueExW(hKey, L"SystemUsesLightTheme", nullptr, nullptr, reinterpret_cast<BYTE*>(&val), &sz);
        RegCloseKey(hKey);
    }
    return (val != 0);
}

static IDWriteTextFormat* CreateTaskbarDWriteFormat(int taskbarH) {
    EnsureDirectWrite();
    if (!g_dwFactory) return nullptr;
    // Exact Windows 10 taskbar clock font: Segoe UI Regular 9pt (12.0 DIP at 40px taskbar height)
    float fontSize = (static_cast<float>(taskbarH) * 12.0f) / 40.0f;
    if (fontSize < 12.0f) fontSize = 12.0f;

    IDWriteTextFormat* fmt = nullptr;
    g_dwFactory->CreateTextFormat(
        L"Segoe UI",
        nullptr,
        DWRITE_FONT_WEIGHT_REGULAR,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        fontSize,
        L"",
        &fmt);
    if (fmt) {
        fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    return fmt;
}

static float MeasureTextDWrite(IDWriteTextFormat* fmt, const wchar_t* text) {
    if (!g_dwFactory || !fmt || !text) return 26.0f;
    IDWriteTextLayout* layout = nullptr;
    HRESULT hr = g_dwFactory->CreateTextLayout(text, static_cast<UINT32>(wcslen(text)), fmt, 200.0f, 40.0f, &layout);
    if (FAILED(hr) || !layout) return 26.0f;
    DWRITE_TEXT_METRICS m = {};
    layout->GetMetrics(&m);
    layout->Release();
    return m.widthIncludingTrailingWhitespace;
}

static void AddRoundedRectPath(GpPath* path, REAL x, REAL y, REAL w, REAL h, REAL r) {
    REAL d = r * 2.0f;
    GdipAddPathArc(path, x, y, d, d, 180.0f, 90.0f);
    GdipAddPathArc(path, x + w - d, y, d, d, 270.0f, 90.0f);
    GdipAddPathArc(path, x + w - d, y + h - d, d, d, 0.0f, 90.0f);
    GdipAddPathArc(path, x, y + h - d, d, d, 90.0f, 90.0f);
    GdipClosePathFigure(path);
}

static void DrawOutlineDeviceIcon(GpGraphics* g, float x, float y, float sz, DeviceType type, ARGB strokeColor) {
    GpPen* pen = nullptr;
    GdipCreatePen1(strokeColor, 1.35f, UnitPixel, &pen);
    if (!pen) return;
    GdipSetPenLineCap197819(pen, LineCapRound, LineCapRound, DashCapRound);
    GdipSetPenLineJoin(pen, LineJoinRound);

    GpPen* thinPen = nullptr;
    GdipCreatePen1(strokeColor, 1.15f, UnitPixel, &thinPen);
    if (thinPen) {
        GdipSetPenLineCap197819(thinPen, LineCapRound, LineCapRound, DashCapRound);
    }

    switch (type) {
        case DeviceType::Mouse: {
            GpPath* p = nullptr;
            GdipCreatePath(FillModeAlternate, &p);
            if (p) {
                float mw = sz * 0.62f;
                float mh = sz * 0.92f;
                float mx = x + (sz - mw) * 0.5f;
                float my = y + (sz - mh) * 0.5f;
                AddRoundedRectPath(p, mx, my, mw, mh, mw * 0.48f);
                GdipDrawPath(g, pen, p);
                GdipDeletePath(p);

                float cx = x + sz * 0.5f;
                GdipDrawLine(g, thinPen ? thinPen : pen, cx, my + 1.0f, cx, my + mh * 0.38f);
            }
            break;
        }
        case DeviceType::Headphones: {
            float cx = x + sz * 0.5f;
            float r = sz * 0.42f;
            GdipDrawArc(g, pen, cx - r, y + 1.5f, r * 2.0f, r * 2.0f, 180.0f, 180.0f);

            float cupW = sz * 0.24f;
            float cupH = sz * 0.42f;
            float cupY = y + sz * 0.48f;
            GpPath* pL = nullptr;
            GdipCreatePath(FillModeAlternate, &pL);
            if (pL) {
                AddRoundedRectPath(pL, cx - r - cupW * 0.15f, cupY, cupW, cupH, 1.4f);
                GdipDrawPath(g, pen, pL);
                GdipDeletePath(pL);
            }
            GpPath* pR = nullptr;
            GdipCreatePath(FillModeAlternate, &pR);
            if (pR) {
                AddRoundedRectPath(pR, cx + r - cupW * 0.85f, cupY, cupW, cupH, 1.4f);
                GdipDrawPath(g, pen, pR);
                GdipDeletePath(pR);
            }
            break;
        }
        case DeviceType::Gamepad: {
            GpPath* p = nullptr;
            GdipCreatePath(FillModeAlternate, &p);
            if (p) {
                float gx = x + 1.0f;
                float gy = y + sz * 0.22f;
                float gw = sz - 2.0f;
                float gh = sz * 0.56f;
                AddRoundedRectPath(p, gx, gy, gw, gh, 3.5f);
                GdipDrawPath(g, pen, p);
                GdipDeletePath(p);
            }
            float cy = y + sz * 0.50f;
            GdipDrawLine(g, thinPen ? thinPen : pen, x + 4.0f, cy, x + 7.0f, cy);
            GdipDrawLine(g, thinPen ? thinPen : pen, x + 5.5f, cy - 1.5f, x + 5.5f, cy + 1.5f);
            GdipDrawEllipse(g, thinPen ? thinPen : pen, x + sz - 6.8f, cy - 1.3f, 2.4f, 2.4f);
            break;
        }
        case DeviceType::Keyboard: {
            GpPath* p = nullptr;
            GdipCreatePath(FillModeAlternate, &p);
            if (p) {
                AddRoundedRectPath(p, x + 0.5f, y + sz * 0.22f, sz - 1.0f, sz * 0.58f, 2.0f);
                GdipDrawPath(g, pen, p);
                GdipDeletePath(p);
            }
            float ky1 = y + sz * 0.40f;
            float ky2 = y + sz * 0.56f;
            float ky3 = y + sz * 0.68f;
            for (float kx = 3.0f; kx <= sz - 4.0f; kx += 3.0f) {
                GdipDrawLine(g, thinPen ? thinPen : pen, x + kx, ky1, x + kx + 1.1f, ky1);
                GdipDrawLine(g, thinPen ? thinPen : pen, x + kx, ky2, x + kx + 1.1f, ky2);
            }
            GdipDrawLine(g, thinPen ? thinPen : pen, x + 4.5f, ky3, x + sz - 4.5f, ky3);
            break;
        }
        case DeviceType::Speaker: {
            GpPath* p = nullptr;
            GdipCreatePath(FillModeAlternate, &p);
            if (p) {
                AddRoundedRectPath(p, x + sz * 0.22f, y + 1.0f, sz * 0.56f, sz - 2.0f, 2.2f);
                GdipDrawPath(g, pen, p);
                GdipDeletePath(p);
            }
            GdipDrawEllipse(g, thinPen ? thinPen : pen, x + sz * 0.40f, y + 3.5f, sz * 0.20f, sz * 0.20f);
            GdipDrawEllipse(g, thinPen ? thinPen : pen, x + sz * 0.34f, y + sz * 0.50f, sz * 0.32f, sz * 0.32f);
            break;
        }
        case DeviceType::Phone: {
            GpPath* p = nullptr;
            GdipCreatePath(FillModeAlternate, &p);
            if (p) {
                AddRoundedRectPath(p, x + sz * 0.24f, y + 0.8f, sz * 0.52f, sz - 1.6f, 2.0f);
                GdipDrawPath(g, pen, p);
                GdipDeletePath(p);
            }
            GdipDrawLine(g, thinPen ? thinPen : pen, x + sz * 0.42f, y + sz - 3.0f, x + sz * 0.58f, y + sz - 3.0f);
            break;
        }
        default: {
            float cx = x + sz * 0.5f;
            float top = y + 1.5f;
            float bot = y + sz - 1.5f;
            float mid = (top + bot) * 0.5f;
            GdipDrawLine(g, pen, cx, top, cx, bot);
            GdipDrawLine(g, pen, cx, top, cx + 4.0f, mid - 2.5f);
            GdipDrawLine(g, pen, cx + 4.0f, mid - 2.5f, cx - 3.5f, mid + 2.5f);
            GdipDrawLine(g, pen, cx, bot, cx + 4.0f, mid + 2.5f);
            GdipDrawLine(g, pen, cx + 4.0f, mid + 2.5f, cx - 3.5f, mid - 2.5f);
            break;
        }
    }

    if (thinPen) GdipDeletePen(thinPen);
    GdipDeletePen(pen);
}

void TaskbarBand_DrawDeviceIconGdip(void* gpGraphics, float x, float y, float sz, DeviceType type, uint32_t argbColor) {
    DrawOutlineDeviceIcon(static_cast<GpGraphics*>(gpGraphics), x, y, sz, type, static_cast<ARGB>(argbColor));
}

static void DrawChargingBolt(GpGraphics* g, float x, float y, ARGB color) {
    GpPen* pen = nullptr;
    GdipCreatePen1(color, 1.3f, UnitPixel, &pen);
    if (!pen) return;
    GdipSetPenLineCap197819(pen, LineCapRound, LineCapRound, DashCapRound);
    GdipSetPenLineJoin(pen, LineJoinRound);
    GdipDrawLine(g, pen, x + 3.5f, y, x + 0.5f, y + 4.5f);
    GdipDrawLine(g, pen, x + 0.5f, y + 4.5f, x + 3.5f, y + 4.5f);
    GdipDrawLine(g, pen, x + 3.5f, y + 4.5f, x + 0.8f, y + 9.0f);
    GdipDeletePen(pen);
}

static void RenderToDib(HDC hdc, int w, int h, const DeviceInfo* devs, int count, bool lightTheme) {
    EnsureDirectWrite();
    IDWriteTextFormat* txtFmt = CreateTaskbarDWriteFormat(h);

    float txtWidths[MAX_TRAY_ICONS] = {};
    wchar_t txts[MAX_TRAY_ICONS][16] = {};
    float itemX[MAX_TRAY_ICONS] = {};
    const float iconSz = 14.0f;

    float curX = 2.0f;
    for (int i = 0; i < count && i < MAX_TRAY_ICONS; ++i) {
        if (devs[i].battery >= 0) swprintf_s(txts[i], ARRAYSIZE(txts[i]), L"%d%%", devs[i].battery);
        else SafeCopyW(txts[i], ARRAYSIZE(txts[i]), L"--%");

        txtWidths[i] = MeasureTextDWrite(txtFmt, txts[i]);
        float boltW = devs[i].charging ? 7.0f : 0.0f;
        float itemW = 6.0f + iconSz + 4.0f + txtWidths[i] + boltW + 6.0f;

        itemX[i] = curX;
        g_itemRects[i].left   = static_cast<LONG>(curX);
        g_itemRects[i].top    = 0;
        g_itemRects[i].right  = static_cast<LONG>(curX + itemW);
        g_itemRects[i].bottom = h;

        curX += itemW;
    }

    // Pass 1: Draw icons, hover sharp rectangle, and alpha=1 hit-testable background via GDI+
    GpGraphics* g = nullptr;
    GdipCreateFromHDC(hdc, &g);
    if (g) {
        GdipSetSmoothingMode(g, SmoothingModeAntiAlias);

        GpSolidFill* hitBg = nullptr;
        GdipCreateSolidFill(Color::MakeARGB(1, 0, 0, 0), &hitBg);
        if (hitBg) {
            GdipFillRectangle(g, hitBg, 0.0f, 0.0f, static_cast<REAL>(w), static_cast<REAL>(h));
            GdipDeleteBrush(hitBg);
        }

        ARGB baseArgb  = lightTheme ? Color::MakeARGB(255, 0, 0, 0) : Color::MakeARGB(255, 255, 255, 255);
        ARGB mutedArgb = lightTheme ? Color::MakeARGB(200, 75, 75, 75) : Color::MakeARGB(200, 185, 185, 185);
        ARGB hoverBg   = lightTheme ? Color::MakeARGB(22, 0, 0, 0) : Color::MakeARGB(26, 255, 255, 255);

        for (int i = 0; i < count && i < MAX_TRAY_ICONS; ++i) {
            float itemW = static_cast<float>(g_itemRects[i].right - g_itemRects[i].left);
            if (i == g_hoverIdx) {
                GpSolidFill* hb = nullptr;
                GdipCreateSolidFill(hoverBg, &hb);
                if (hb) {
                    GdipFillRectangle(g, hb, itemX[i], 0.0f, itemW, static_cast<REAL>(h));
                    GdipDeleteBrush(hb);
                }
            }

            ARGB iconCol = (devs[i].sleeping || devs[i].battery < 0) ? mutedArgb : baseArgb;
            float iconY = (static_cast<float>(h) - iconSz) * 0.5f;
            DrawOutlineDeviceIcon(g, itemX[i] + 6.0f, iconY, iconSz, devs[i].type, iconCol);

            if (devs[i].charging) {
                DrawChargingBolt(g, itemX[i] + 6.0f + iconSz + 4.0f + txtWidths[i] + 1.0f,
                                 (static_cast<float>(h) - 9.0f) * 0.5f, baseArgb);
            }
        }
        GdipDeleteGraphics(g);
    }
    GdiFlush();

    // Pass 2: Draw percentage text using native Windows 10 Direct2D + DirectWrite (identical to taskbar clock!)
    if (g_d2dRT && txtFmt) {
        RECT bindRc = { 0, 0, w, h };
        if (SUCCEEDED(g_d2dRT->BindDC(hdc, &bindRc))) {
            g_d2dRT->BeginDraw();
            g_d2dRT->SetTransform(D2D1::Matrix3x2F::Identity());

            ID2D1SolidColorBrush* brMain = nullptr;
            ID2D1SolidColorBrush* brMuted = nullptr;
            D2D1_COLOR_F cMain  = lightTheme ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f);
            D2D1_COLOR_F cMuted = lightTheme ? D2D1::ColorF(0.3f, 0.3f, 0.3f, 0.8f) : D2D1::ColorF(0.75f, 0.75f, 0.75f, 0.85f);
            g_d2dRT->CreateSolidColorBrush(cMain, &brMain);
            g_d2dRT->CreateSolidColorBrush(cMuted, &brMuted);

            for (int i = 0; i < count && i < MAX_TRAY_ICONS; ++i) {
                ID2D1SolidColorBrush* br = (devs[i].sleeping || devs[i].battery < 0) ? brMuted : brMain;
                if (!br) continue;
                float tx = itemX[i] + 6.0f + iconSz + 4.0f;
                D2D1_RECT_F tr = D2D1::RectF(tx, 0.0f, tx + txtWidths[i] + 6.0f, static_cast<float>(h));
                g_d2dRT->DrawText(txts[i], static_cast<UINT32>(wcslen(txts[i])), txtFmt, tr, br);
            }

            if (brMain) brMain->Release();
            if (brMuted) brMuted->Release();
            if (g_d2dRT->EndDraw() == static_cast<HRESULT>(D2DERR_RECREATE_TARGET)) {
                g_d2dRT->Release();
                g_d2dRT = nullptr;
            }
        }
    }

    if (txtFmt) txtFmt->Release();
}

void TaskbarBand_RenderSampleToDc(HDC hdc, int w, int h, const DeviceInfo* devs, int count, bool lightTheme, COLORREF) {
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (mem && bmp && bits) {
        HGDIOBJ ob = SelectObject(mem, bmp);
        ZeroMemory(bits, static_cast<size_t>(w) * h * 4);
        RenderToDib(mem, w, h, devs, count, lightTheme);
        BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        AlphaBlend(hdc, 0, 0, w, h, mem, 0, 0, w, h, bf);
        SelectObject(mem, ob);
    }
    if (bmp) DeleteObject(bmp);
    if (mem) DeleteDC(mem);
}

static int ComputeRequiredWidth(const DeviceInfo* devs, int count, int h) {
    if (count <= 0) return 0;
    IDWriteTextFormat* txtFmt = CreateTaskbarDWriteFormat(h);

    float total = 2.0f;
    for (int i = 0; i < count && i < MAX_TRAY_ICONS; ++i) {
        wchar_t txt[16] = {};
        if (devs[i].battery >= 0) swprintf_s(txt, ARRAYSIZE(txt), L"%d%%", devs[i].battery);
        else SafeCopyW(txt, ARRAYSIZE(txt), L"--%");

        float txtW = MeasureTextDWrite(txtFmt, txt);
        float boltW = devs[i].charging ? 7.0f : 0.0f;
        total += (6.0f + 14.0f + 4.0f + txtW + boltW + 6.0f);
    }
    total += 2.0f;

    if (txtFmt) txtFmt->Release();
    return static_cast<int>(total + 1.0f);
}

static void RenderLayeredBand() {
    if (!g_bandWnd || g_bandCount <= 0 || g_bandWidth <= 0 || g_bandHeight <= 0) return;

    g_lightTheme = IsTaskbarLightTheme();

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = g_bandWidth;
    bmi.bmiHeader.biHeight = -g_bandHeight;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HDC screenDc = GetDC(nullptr);
    HDC memDc = CreateCompatibleDC(screenDc);
    HBITMAP hBmp = CreateDIBSection(screenDc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (memDc && hBmp && bits) {
        HGDIOBJ oldBmp = SelectObject(memDc, hBmp);
        ZeroMemory(bits, static_cast<size_t>(g_bandWidth) * g_bandHeight * 4);
        RenderToDib(memDc, g_bandWidth, g_bandHeight, g_bandDevs, g_bandCount, g_lightTheme);

        SIZE sz = { g_bandWidth, g_bandHeight };
        POINT ptSrc = { 0, 0 };
        BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        UpdateLayeredWindow(g_bandWnd, screenDc, nullptr, &sz, memDc, &ptSrc, 0, &bf, ULW_ALPHA);

        SelectObject(memDc, oldBmp);
    }
    if (hBmp) DeleteObject(hBmp);
    if (memDc) DeleteDC(memDc);
    ReleaseDC(nullptr, screenDc);
}

static void RestoreRebarWidth() {
    if (g_lastRebarHwnd && IsWindow(g_lastRebarHwnd) && g_lastRebarShrink > 0) {
        HWND hTray = FindWindowW(L"Shell_TrayWnd", nullptr);
        HWND hNotify = hTray ? FindWindowExW(hTray, nullptr, L"TrayNotifyWnd", nullptr) : nullptr;
        RECT rcRebar = {}, rcNotify = {};
        if (hNotify && GetWindowRect(g_lastRebarHwnd, &rcRebar) && GetWindowRect(hNotify, &rcNotify)) {
            int fullW = rcNotify.left - rcRebar.left;
            if (fullW > (rcRebar.right - rcRebar.left)) {
                SetWindowPos(g_lastRebarHwnd, nullptr, 0, 0, fullW, rcRebar.bottom - rcRebar.top,
                             SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
        }
    }
    g_lastRebarShrink = 0;
}

void TaskbarBand_SyncPosition() {
    if (!g_bandWnd || !IsWindow(g_bandWnd)) return;

    if (g_bandCount <= 0) {
        if (g_bandVisible || IsWindowVisible(g_bandWnd)) {
            ShowWindow(g_bandWnd, SW_HIDE);
            g_bandVisible = false;
        }
        RestoreRebarWidth();
        return;
    }

    HWND hTray = FindWindowW(L"Shell_TrayWnd", nullptr);
    HWND hNotify = hTray ? FindWindowExW(hTray, nullptr, L"TrayNotifyWnd", nullptr) : nullptr;
    HWND hRebar = hTray ? FindWindowExW(hTray, nullptr, L"ReBarWindow32", nullptr) : nullptr;

    if (!hTray || !hNotify || !IsWindowVisible(hTray)) {
        if (g_bandVisible || IsWindowVisible(g_bandWnd)) {
            ShowWindow(g_bandWnd, SW_HIDE);
            g_bandVisible = false;
        }
        return;
    }

    if (g_ownerTray != hTray) {
        SetWindowLongPtrW(g_bandWnd, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(hTray));
        g_ownerTray = hTray;
        g_bandVisible = false;
    }

    RECT rcTray = {}, rcNotify = {};
    if (!GetWindowRect(hTray, &rcTray) || !GetWindowRect(hNotify, &rcNotify)) return;

    int trayH = rcTray.bottom - rcTray.top;
    if (trayH < 24 || trayH > 120) {
        if (g_bandVisible || IsWindowVisible(g_bandWnd)) {
            ShowWindow(g_bandWnd, SW_HIDE);
            g_bandVisible = false;
        }
        return;
    }

    bool light = IsTaskbarLightTheme();
    bool themeChanged = (light != g_lightTheme);
    bool sizeChanged = (trayH != g_bandHeight);
    g_bandHeight = trayH;
    int neededW = ComputeRequiredWidth(g_bandDevs, g_bandCount, g_bandHeight);
    if (neededW != g_bandWidth) sizeChanged = true;
    g_bandWidth = neededW;

    if (hRebar) {
        RECT rcRebar = {};
        if (GetWindowRect(hRebar, &rcRebar)) {
            int maxAllowedRight = rcNotify.left - g_bandWidth - 6;
            if (rcRebar.right > maxAllowedRight && maxAllowedRight > rcRebar.left + 80) {
                int newRebarW = maxAllowedRight - rcRebar.left;
                SetWindowPos(hRebar, nullptr, 0, 0, newRebarW, rcRebar.bottom - rcRebar.top,
                             SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
                g_lastRebarHwnd = hRebar;
                g_lastRebarShrink = rcNotify.left - maxAllowedRight;
            }
        }
    }

    int targetX = rcNotify.left - g_bandWidth - 2;
    int targetY = rcTray.top;

    if (sizeChanged || themeChanged) {
        RenderLayeredBand();
    }

    if (!g_bandVisible || !IsWindowVisible(g_bandWnd) ||
        targetX != g_lastPosX || targetY != g_lastPosY ||
        g_bandWidth != g_lastPosW || g_bandHeight != g_lastPosH) {
        SetWindowPos(g_bandWnd, HWND_TOPMOST, targetX, targetY, g_bandWidth, g_bandHeight,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        g_lastPosX = targetX;
        g_lastPosY = targetY;
        g_lastPosW = g_bandWidth;
        g_lastPosH = g_bandHeight;
        g_bandVisible = true;
    }
}

static int HitTestItem(int x, int y) {
    POINT pt = { x, y };
    for (int i = 0; i < g_bandCount; ++i) {
        if (PtInRect(&g_itemRects[i], pt)) return i;
    }
    return -1;
}

static void UpdateHoverTooltip(int idx) {
    if (!g_tipWnd) return;
    TOOLINFOW ti = {};
    ti.cbSize = sizeof(ti);
    ti.hwnd = g_bandWnd;
    ti.uId = reinterpret_cast<UINT_PTR>(g_bandWnd);

    if (idx < 0 || idx >= g_bandCount) {
        SendMessageW(g_tipWnd, TTM_ACTIVATE, FALSE, 0);
        return;
    }

    const DeviceInfo& d = g_bandDevs[idx];
    wchar_t tipBuf[192] = {};
    if (d.battery >= 0) {
        swprintf_s(tipBuf, ARRAYSIZE(tipBuf), L"%ls: %d%%%ls (%ls)",
                   d.name, d.battery,
                   d.charging ? L" • зарядка" : (d.sleeping ? L" • спит" : L""),
                   ConnectionKindLabel(d.conn));
    } else {
        swprintf_s(tipBuf, ARRAYSIZE(tipBuf), L"%ls: пошевелите мышкой (%ls)",
                   d.name, ConnectionKindLabel(d.conn));
    }
    ti.lpszText = tipBuf;
    SendMessageW(g_tipWnd, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&ti));
    SendMessageW(g_tipWnd, TTM_ACTIVATE, TRUE, 0);
}

static LRESULT CALLBACK BandWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_TIMER:
            if (wParam == BAND_SYNC_TIMER_ID) {
                TaskbarBand_SyncPosition();
            }
            return 0;

        case WM_MOUSEMOVE: {
            if (!g_trackingMouse) {
                TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
                TrackMouseEvent(&tme);
                g_trackingMouse = true;
            }
            int idx = HitTestItem(static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)));
            if (idx != g_hoverIdx) {
                g_hoverIdx = idx;
                UpdateHoverTooltip(idx);
                RenderLayeredBand();
            }
            return 0;
        }

        case WM_MOUSELEAVE:
            g_trackingMouse = false;
            if (g_hoverIdx != -1) {
                g_hoverIdx = -1;
                UpdateHoverTooltip(-1);
                RenderLayeredBand();
            }
            return 0;

        case WM_LBUTTONUP:
        case WM_RBUTTONUP:
            if (g_mainWnd) {
                PostMessageW(g_mainWnd, WM_APP + 122, 0, 0);
            }
            return 0;

        case WM_SETTINGCHANGE:
            RenderLayeredBand();
            TaskbarBand_SyncPosition();
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void TaskbarBand_Init(HINSTANCE hInst, HWND mainWnd) {
    g_hInst = hInst;
    g_mainWnd = mainWnd;

    GdiplusStartupInput gsi;
    GdiplusStartup(&g_gdipToken, &gsi, nullptr);
    EnsureDirectWrite();

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = BandWndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"TrayBatteryTaskbarBand";
    wc.hCursor = LoadCursorW(nullptr, IDC_HAND);
    RegisterClassExW(&wc);

    g_bandWnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        L"TrayBatteryTaskbarBand",
        L"",
        WS_POPUP,
        0, 0, 10, 40,
        nullptr, nullptr, hInst, nullptr);

    if (g_bandWnd) {
        g_tipWnd = CreateWindowExW(
            WS_EX_TOPMOST,
            TOOLTIPS_CLASSW,
            nullptr,
            WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
            CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
            g_bandWnd, nullptr, hInst, nullptr);
        if (g_tipWnd) {
            TOOLINFOW ti = {};
            ti.cbSize = sizeof(ti);
            ti.uFlags = TTF_SUBCLASS | TTF_IDISHWND;
            ti.hwnd = g_bandWnd;
            ti.uId = reinterpret_cast<UINT_PTR>(g_bandWnd);
            ti.lpszText = const_cast<LPWSTR>(L"TrayBattery");
            SendMessageW(g_tipWnd, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
            SendMessageW(g_tipWnd, TTM_SETMAXTIPWIDTH, 0, 320);
        }
        SetTimer(g_bandWnd, BAND_SYNC_TIMER_ID, BAND_SYNC_MS, nullptr);
    }
}

void TaskbarBand_Update(const DeviceSnapshot* snapshot) {
    DeviceInfo next[MAX_TRAY_ICONS] = {};
    int nextCount = 0;
    if (snapshot) {
        for (int i = 0; i < snapshot->count && nextCount < MAX_TRAY_ICONS; ++i) {
            if (snapshot->items[i].connected) {
                next[nextCount++] = snapshot->items[i];
            }
        }
    }

    bool changed = (nextCount != g_bandCount);
    if (!changed) {
        for (int i = 0; i < nextCount; ++i) {
            if (wcscmp(next[i].id, g_bandDevs[i].id) != 0 ||
                next[i].type != g_bandDevs[i].type ||
                next[i].battery != g_bandDevs[i].battery ||
                next[i].charging != g_bandDevs[i].charging ||
                next[i].sleeping != g_bandDevs[i].sleeping) {
                changed = true;
                break;
            }
        }
    }

    for (int i = 0; i < nextCount; ++i) g_bandDevs[i] = next[i];
    g_bandCount = nextCount;

    if (changed) {
        g_bandWidth = ComputeRequiredWidth(g_bandDevs, g_bandCount, g_bandHeight);
        RenderLayeredBand();
        TaskbarBand_SyncPosition();
    }
}

void TaskbarBand_Shutdown() {
    RestoreRebarWidth();
    if (g_bandWnd) {
        KillTimer(g_bandWnd, BAND_SYNC_TIMER_ID);
        DestroyWindow(g_bandWnd);
        g_bandWnd = nullptr;
    }
    ReleaseDirectWrite();
    if (g_gdipToken) {
        GdiplusShutdown(g_gdipToken);
        g_gdipToken = 0;
    }
}
