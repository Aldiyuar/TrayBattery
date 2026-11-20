#include "flyout.h"
#include "autostart.h"
#include "taskbar_band.h"
#include <objidl.h>
#include <gdiplus.h>

using namespace Gdiplus;
using namespace Gdiplus::DllExports;

enum FlyoutAction : int {
    ACT_NONE = -1,
    ACT_AUTOSTART = 0,
    ACT_REFRESH   = 1,
    ACT_EXIT      = 2
};

static HINSTANCE g_hInst = nullptr;
static HWND g_flyoutWnd = nullptr;
static FlyoutCallbacks g_cb = {};
static DeviceSnapshot g_snap = {};
static ULONGLONG g_hideTick = 0;

static HFONT g_fName = nullptr;
static HFONT g_fPct  = nullptr;
static HFONT g_fMenu = nullptr;
static HFONT g_fSub  = nullptr;

static int g_hoverAct = ACT_NONE;
static bool g_tracking = false;
static int g_scrollY = 0;
static int g_maxScroll = 0;

static RECT g_rcActions[3] = {};
static int g_listTop = 0;
static int g_listBottom = 0;
static int g_winW = 276;
static int g_winH = 156;

static constexpr int FLYOUT_W = 276;
static constexpr int DEV_ROW_H = 58;
static constexpr int MAX_VISIBLE_DEVS = 5;
static constexpr int ACT_ROW_H = 28;
static constexpr int PAD_X = 14;

static bool IsSystemLightTheme() {
    HKEY hKey = nullptr;
    DWORD val = 0;
    DWORD sz = sizeof(val);
    bool got = false;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                      0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        if (RegQueryValueExW(hKey, L"SystemUsesLightTheme", nullptr, nullptr,
                             reinterpret_cast<BYTE*>(&val), &sz) == ERROR_SUCCESS) {
            got = true;
        } else {
            sz = sizeof(val);
            if (RegQueryValueExW(hKey, L"AppsUseLightTheme", nullptr, nullptr,
                                 reinterpret_cast<BYTE*>(&val), &sz) == ERROR_SUCCESS) {
                got = true;
            }
        }
        RegCloseKey(hKey);
    }
    return got ? (val != 0) : false;
}

static COLORREF GetWindowsAccentColor() {
    HKEY hKey = nullptr;
    DWORD abgr = 0;
    DWORD sz = sizeof(abgr);
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        LSTATUS st = RegQueryValueExW(hKey, L"AccentColor", nullptr, nullptr, reinterpret_cast<BYTE*>(&abgr), &sz);
        RegCloseKey(hKey);
        if (st == ERROR_SUCCESS && abgr != 0) {
            return RGB(abgr & 0xFF, (abgr >> 8) & 0xFF, (abgr >> 16) & 0xFF);
        }
    }
    return RGB(0, 120, 215);
}

static void EnsureFonts() {
    if (!g_fName) {
        // Native Windows 10 flyout typography: all Segoe UI Regular (no bold, no custom colors)
        g_fName = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        g_fPct  = CreateFontW(-19, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        g_fMenu = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        g_fSub  = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    }
}

static void RecalcLayout() {
    int devCount = 0;
    for (int i = 0; i < g_snap.count; ++i) {
        if (g_snap.items[i].connected) devCount++;
    }

    int visRows = (devCount == 0) ? 1 : ((devCount > MAX_VISIBLE_DEVS) ? MAX_VISIBLE_DEVS : devCount);
    int listH = (devCount == 0) ? 44 : (visRows * DEV_ROW_H);
    int totalH = devCount * DEV_ROW_H;
    g_maxScroll = (totalH > listH) ? (totalH - listH) : 0;
    if (g_scrollY > g_maxScroll) g_scrollY = g_maxScroll;
    if (g_scrollY < 0) g_scrollY = 0;

    g_winW = FLYOUT_W;
    g_listTop = 8;
    g_listBottom = g_listTop + listH;
    int y = g_listBottom + 7; // divider space

    for (int i = 0; i < 3; ++i) {
        g_rcActions[i] = { 1, y, g_winW - 1, y + ACT_ROW_H };
        y += ACT_ROW_H;
    }
    g_winH = y + 6;
}

static void PaintFlyout(HWND hwnd) {
    PAINTSTRUCT ps = {};
    HDC hdc = BeginPaint(hwnd, &ps);

    RECT rc = {};
    GetClientRect(hwnd, &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;

    HDC memDc = CreateCompatibleDC(hdc);
    HBITMAP memBmp = CreateCompatibleBitmap(hdc, w, h);
    HGDIOBJ oldBmp = SelectObject(memDc, memBmp);

    bool light = IsSystemLightTheme();
    COLORREF accent = GetWindowsAccentColor();

    // Standard Windows 10 flyout palette (matches native volume / network flyouts)
    COLORREF bgCol     = light ? RGB(242, 242, 242) : RGB(31, 31, 31);
    COLORREF borderCol = light ? RGB(204, 204, 204) : RGB(56, 56, 56);
    COLORREF divCol    = light ? RGB(218, 218, 218) : RGB(52, 52, 52);
    COLORREF hoverCol  = light ? RGB(224, 224, 224) : RGB(51, 51, 51);
    COLORREF txtMain   = light ? RGB(0, 0, 0)       : RGB(255, 255, 255);
    COLORREF txtSub    = light ? RGB(102, 102, 102) : RGB(166, 166, 166);
    COLORREF trackCol  = light ? RGB(153, 153, 153) : RGB(118, 118, 118);

    HBRUSH bgBr = CreateSolidBrush(bgCol);
    FillRect(memDc, &rc, bgBr);
    DeleteObject(bgBr);

    // Subtle 1px standard Windows 10 border (no drop shadow, flush with taskbar)
    HBRUSH brdBr = CreateSolidBrush(borderCol);
    FrameRect(memDc, &rc, brdBr);
    DeleteObject(brdBr);

    SetBkMode(memDc, TRANSPARENT);

    const DeviceInfo* devs[MAX_DEVICES] = {};
    int devCount = 0;
    for (int i = 0; i < g_snap.count; ++i) {
        if (g_snap.items[i].connected) devs[devCount++] = &g_snap.items[i];
    }

    HRGN clip = CreateRectRgn(1, g_listTop, w - 1, g_listBottom);
    SelectClipRgn(memDc, clip);

    if (devCount == 0) {
        SelectObject(memDc, g_fSub);
        SetTextColor(memDc, txtSub);
        RECT er = { PAD_X, g_listTop, w - PAD_X, g_listBottom };
        DrawTextW(memDc, L"Нет подключённых устройств", -1, &er, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    } else {
        GpGraphics* gp = nullptr;
        GdipCreateFromHDC(memDc, &gp);
        if (gp) GdipSetSmoothingMode(gp, SmoothingModeAntiAlias);

        ARGB iconArgb = Color::MakeARGB(255, GetRValue(txtMain), GetGValue(txtMain), GetBValue(txtMain));

        for (int i = 0; i < devCount; ++i) {
            const DeviceInfo& d = *devs[i];
            int ry = g_listTop + i * DEV_ROW_H - g_scrollY;
            if (ry + DEV_ROW_H < g_listTop || ry > g_listBottom) continue;

            // Row 1: Device name in standard white/black Segoe UI Regular (like volume output device header)
            wchar_t title[160] = {};
            if (d.charging) {
                swprintf_s(title, ARRAYSIZE(title), L"%ls (зарядка)", d.name);
            } else if (d.sleeping) {
                swprintf_s(title, ARRAYSIZE(title), L"%ls (спит)", d.name);
            } else {
                SafeCopyW(title, ARRAYSIZE(title), d.name);
            }

            SelectObject(memDc, g_fName);
            SetTextColor(memDc, txtMain);
            RECT nr = { PAD_X, ry + 4, w - PAD_X, ry + 24 };
            DrawTextW(memDc, title, -1, &nr, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

            // Row 2: [Icon]  [========------]  80%  (exact Windows 10 volume slider layout)
            int midY = ry + 39;

            if (gp) {
                TaskbarBand_DrawDeviceIconGdip(gp, static_cast<float>(PAD_X), static_cast<float>(midY - 9),
                                               18.0f, d.type, iconArgb);
            }

            // Battery number on the right (plain Segoe UI Regular in standard theme color)
            wchar_t pct[16] = {};
            if (d.battery >= 0) swprintf_s(pct, ARRAYSIZE(pct), L"%d%%", d.battery);
            else SafeCopyW(pct, ARRAYSIZE(pct), L"--");

            SelectObject(memDc, g_fPct);
            SetTextColor(memDc, txtMain);
            RECT pr = { w - PAD_X - 48, ry + 27, w - PAD_X, ry + 51 };
            DrawTextW(memDc, pct, -1, &pr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

            // Thin 2px horizontal bar (Windows accent color for filled portion, gray for remaining)
            int bx = PAD_X + 28;
            int bw = (w - PAD_X - 54) - bx;
            if (bw > 20) {
                HBRUSH trBr = CreateSolidBrush(trackCol);
                RECT trRc = { bx, midY - 1, bx + bw, midY + 1 };
                FillRect(memDc, &trRc, trBr);
                DeleteObject(trBr);

                if (d.battery > 0) {
                    int fw = (bw * d.battery) / 100;
                    if (fw < 2) fw = 2;
                    HBRUSH flBr = CreateSolidBrush(accent);
                    RECT flRc = { bx, midY - 1, bx + fw, midY + 1 };
                    FillRect(memDc, &flRc, flBr);
                    DeleteObject(flBr);
                }
            }
        }
        if (gp) GdipDeleteGraphics(gp);
    }

    SelectClipRgn(memDc, nullptr);
    DeleteObject(clip);

    // Subtle divider
    HPEN divPen = CreatePen(PS_SOLID, 1, divCol);
    HGDIOBJ op = SelectObject(memDc, divPen);
    MoveToEx(memDc, PAD_X, g_listBottom + 3, nullptr);
    LineTo(memDc, w - PAD_X, g_listBottom + 3);
    SelectObject(memDc, op);
    DeleteObject(divPen);

    // Plain standard Windows 10 text action items (no checkboxes, no colored icons)
    bool autoOn = IsAutoStartEnabled();
    const wchar_t* labels[3] = {
        L"Автозапуск с Windows",
        L"Обновить",
        L"Выход"
    };

    for (int i = 0; i < 3; ++i) {
        const RECT& ar = g_rcActions[i];
        if (i == g_hoverAct) {
            HBRUSH hBr = CreateSolidBrush(hoverCol);
            FillRect(memDc, &ar, hBr);
            DeleteObject(hBr);
        }

        SelectObject(memDc, g_fMenu);
        SetTextColor(memDc, txtMain);
        RECT tr = { PAD_X, ar.top, (i == ACT_AUTOSTART) ? (w - PAD_X - 42) : (w - PAD_X), ar.bottom };
        DrawTextW(memDc, labels[i], -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        if (i == ACT_AUTOSTART) {
            SelectObject(memDc, g_fSub);
            SetTextColor(memDc, txtSub);
            RECT sr = { w - PAD_X - 42, ar.top, w - PAD_X, ar.bottom };
            DrawTextW(memDc, autoOn ? L"Вкл" : L"Выкл", -1, &sr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        }
    }

    BitBlt(hdc, 0, 0, w, h, memDc, 0, 0, SRCCOPY);
    SelectObject(memDc, oldBmp);
    DeleteObject(memBmp);
    DeleteDC(memDc);
    EndPaint(hwnd, &ps);
}

static int HitTestAction(int x, int y) {
    POINT pt = { x, y };
    for (int i = 0; i < 3; ++i) {
        if (PtInRect(&g_rcActions[i], pt)) return i;
    }
    return ACT_NONE;
}

static LRESULT CALLBACK FlyoutWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT:
            PaintFlyout(hwnd);
            return 0;

        case WM_ACTIVATE:
            if (LOWORD(wParam) == WA_INACTIVE) {
                g_hideTick = GetTickCount64();
                ShowWindow(hwnd, SW_HIDE);
            }
            return 0;

        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) {
                ShowWindow(hwnd, SW_HIDE);
                return 0;
            }
            break;

        case WM_MOUSEMOVE: {
            if (!g_tracking) {
                TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
                TrackMouseEvent(&tme);
                g_tracking = true;
            }
            int act = HitTestAction(static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)));
            if (act != g_hoverAct) {
                g_hoverAct = act;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }

        case WM_MOUSELEAVE:
            g_tracking = false;
            if (g_hoverAct != ACT_NONE) {
                g_hoverAct = ACT_NONE;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;

        case WM_MOUSEWHEEL: {
            if (g_maxScroll > 0) {
                short delta = GET_WHEEL_DELTA_WPARAM(wParam);
                g_scrollY -= (delta / WHEEL_DELTA) * 28;
                if (g_scrollY < 0) g_scrollY = 0;
                if (g_scrollY > g_maxScroll) g_scrollY = g_maxScroll;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }

        case WM_LBUTTONUP: {
            int act = HitTestAction(static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)));
            if (act == ACT_AUTOSTART) {
                if (g_cb.onToggleAutoStart) g_cb.onToggleAutoStart(!IsAutoStartEnabled());
                InvalidateRect(hwnd, nullptr, FALSE);
            } else if (act == ACT_REFRESH) {
                if (g_cb.onRefresh) g_cb.onRefresh();
                InvalidateRect(hwnd, nullptr, FALSE);
            } else if (act == ACT_EXIT) {
                ShowWindow(hwnd, SW_HIDE);
                if (g_cb.onQuit) g_cb.onQuit();
            }
            return 0;
        }

        case WM_SETTINGCHANGE:
            if (IsWindowVisible(hwnd)) {
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void Flyout_Init(HINSTANCE hInst, HWND ownerWnd, const FlyoutCallbacks& cb) {
    g_hInst = hInst;
    g_cb = cb;
    EnsureFonts();

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = FlyoutWndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"TrayBatteryFlyoutWnd";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    g_flyoutWnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        L"TrayBatteryFlyoutWnd",
        L"",
        WS_POPUP,
        0, 0, FLYOUT_W, 156,
        ownerWnd, nullptr, hInst, nullptr);
}

bool Flyout_WillOpenOnToggle() {
    if (!g_flyoutWnd) return false;
    if (IsWindowVisible(g_flyoutWnd)) return false;
    if ((GetTickCount64() - g_hideTick) < 220ULL) return false;
    return true;
}

void Flyout_Toggle(const DeviceSnapshot* snapshot, POINT anchorPt) {
    if (!g_flyoutWnd) return;

    if (IsWindowVisible(g_flyoutWnd)) {
        ShowWindow(g_flyoutWnd, SW_HIDE);
        return;
    }

    if ((GetTickCount64() - g_hideTick) < 220ULL) {
        return;
    }

    if (snapshot) g_snap = *snapshot;
    g_hoverAct = ACT_NONE;
    RecalcLayout();

    RECT wa = {};
    HMONITOR mon = MonitorFromPoint(anchorPt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(mon, &mi)) {
        wa = mi.rcWork;
    } else {
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    }

    int x = anchorPt.x - g_winW / 2;
    if (x + g_winW > wa.right) x = wa.right - g_winW;
    if (x < wa.left) x = wa.left;
    int y = wa.bottom - g_winH;

    // Dock flush against the top edge of the Windows 10 taskbar (exact native volume flyout alignment)
    HWND hTray = FindWindowW(L"Shell_TrayWnd", nullptr);
    RECT rcTray = {};
    if (hTray && GetWindowRect(hTray, &rcTray) && rcTray.top > wa.top && (rcTray.bottom - rcTray.top) < 150) {
        y = rcTray.top - g_winH;
    }

    SetWindowPos(g_flyoutWnd, HWND_TOPMOST, x, y, g_winW, g_winH, SWP_SHOWWINDOW);
    SetForegroundWindow(g_flyoutWnd);
    InvalidateRect(g_flyoutWnd, nullptr, FALSE);
}

void Flyout_UpdateIfVisible(const DeviceSnapshot* snapshot) {
    if (snapshot) g_snap = *snapshot;
    if (!g_flyoutWnd || !IsWindowVisible(g_flyoutWnd)) return;

    int oldH = g_winH;
    RecalcLayout();
    if (g_winH != oldH) {
        RECT r = {};
        GetWindowRect(g_flyoutWnd, &r);
        int dy = oldH - g_winH;
        SetWindowPos(g_flyoutWnd, HWND_TOPMOST, r.left, r.top + dy, g_winW, g_winH, SWP_NOACTIVATE);
    }
    InvalidateRect(g_flyoutWnd, nullptr, FALSE);
}

void Flyout_Hide() {
    if (g_flyoutWnd && IsWindowVisible(g_flyoutWnd)) {
        ShowWindow(g_flyoutWnd, SW_HIDE);
    }
}

bool Flyout_IsVisible() {
    return (g_flyoutWnd && IsWindowVisible(g_flyoutWnd));
}

void Flyout_Shutdown() {
    if (g_flyoutWnd) {
        DestroyWindow(g_flyoutWnd);
        g_flyoutWnd = nullptr;
    }
    if (g_fName) { DeleteObject(g_fName); g_fName = nullptr; }
    if (g_fPct)  { DeleteObject(g_fPct);  g_fPct = nullptr; }
    if (g_fMenu) { DeleteObject(g_fMenu); g_fMenu = nullptr; }
    if (g_fSub)  { DeleteObject(g_fSub);  g_fSub = nullptr; }
}
