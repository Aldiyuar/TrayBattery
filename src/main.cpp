#include "common.h"
#include "autostart.h"
#include "hid_monitor.h"
#include "bt_monitor.h"
#include "tray_icons.h"
#include "taskbar_band.h"
#include "flyout.h"
#include <dbt.h>
#include <shellapi.h>
#include <hidsdi.h>

static HWND g_mainHwnd = nullptr;
static UINT g_msgTaskbarCreated = 0;
static HDEVNOTIFY g_hDevNotifyHid = nullptr;
static DeviceSnapshot g_lastSnap = {};
static HANDLE g_singleMutex = nullptr;

static int CompareDevices(const DeviceInfo& a, const DeviceInfo& b) {
    if (a.connected != b.connected) return a.connected ? -1 : 1;
    bool aHasBat = (a.battery >= 0);
    bool bHasBat = (b.battery >= 0);
    if (aHasBat != bHasBat) return aHasBat ? -1 : 1;
    if (a.conn != b.conn) {
        if (a.conn == ConnectionKind::Dongle24G) return -1;
        if (b.conn == ConnectionKind::Dongle24G) return 1;
    }
    int nameCmp = _wcsicmp(a.name, b.name);
    if (nameCmp != 0) return nameCmp;
    return _wcsicmp(a.id, b.id);
}

static void SortSnapshot(DeviceSnapshot* snap) {
    for (int i = 1; i < snap->count; ++i) {
        DeviceInfo key = snap->items[i];
        int j = i - 1;
        while (j >= 0 && CompareDevices(key, snap->items[j]) < 0) {
            snap->items[j + 1] = snap->items[j];
            j--;
        }
        snap->items[j + 1] = key;
    }
}

static void RefreshAll() {
    static DeviceSnapshot snap;
    ZeroMemory(&snap, sizeof(snap));
    HidMonitor_CollectDevices(&snap);
    BtMonitor_CollectDevices(&snap, false);
    SortSnapshot(&snap);

    g_lastSnap = snap;
    TaskbarBand_Update(&g_lastSnap);
    TrayIcons_Update(&g_lastSnap);
    Flyout_UpdateIfVisible(&g_lastSnap);
}

static void OnRefresh() {
    HidMonitor_TriggerRescan();
    RefreshAll();
}

static void OnToggleAutoStart(bool enabled) {
    SetAutoStartEnabled(enabled);
}

static void OnQuit() {
    DestroyWindow(g_mainHwnd);
}

static constexpr UINT_PTR TIMER_DEBOUNCE_ID = 2;
static constexpr UINT DEBOUNCE_MS = 350;

static void ToggleFlyoutAtCursor() {
    POINT pt = {};
    GetCursorPos(&pt);
    if (Flyout_WillOpenOnToggle()) {
        RefreshAll();
    }
    Flyout_Toggle(&g_lastSnap, pt);
}

static LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (g_msgTaskbarCreated != 0 && msg == g_msgTaskbarCreated) {
        TrayIcons_ReAddAll();
        TaskbarBand_SyncPosition();
        return 0;
    }

    if (msg >= WM_APP_TRAY_BASE && msg < WM_APP_TRAY_BASE + MAX_TRAY_ICONS) {
        UINT ev = LOWORD(lParam);
        if (ev == WM_LBUTTONUP || ev == WM_RBUTTONUP) {
            ToggleFlyoutAtCursor();
        }
        return 0;
    }

    switch (msg) {
        case WM_CREATE: {
            GUID hidGuid;
            HidD_GetHidGuid(&hidGuid);
            DEV_BROADCAST_DEVICEINTERFACE_W dbi = {};
            dbi.dbcc_size = sizeof(dbi);
            dbi.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
            dbi.dbcc_classguid = hidGuid;
            g_hDevNotifyHid = RegisterDeviceNotificationW(hwnd, &dbi,
                DEVICE_NOTIFY_WINDOW_HANDLE | DEVICE_NOTIFY_ALL_INTERFACE_CLASSES);

            SetTimer(hwnd, TIMER_POLL_ID, POLL_INTERVAL_MS, nullptr);
            return 0;
        }

        case WM_TIMER:
            if (wParam == TIMER_POLL_ID) {
                RefreshAll();
            } else if (wParam == TIMER_DEBOUNCE_ID) {
                KillTimer(hwnd, TIMER_DEBOUNCE_ID);
                HidMonitor_TriggerRescan();
                RefreshAll();
            }
            return 0;

        case WM_APP_HID_UPDATE:
            RefreshAll();
            return 0;

        case WM_APP + 122:
            ToggleFlyoutAtCursor();
            return 0;

        case WM_DEVICECHANGE:
            if (wParam == DBT_DEVICEARRIVAL || wParam == DBT_DEVICEREMOVECOMPLETE || wParam == DBT_DEVNODES_CHANGED) {
                SetTimer(hwnd, TIMER_DEBOUNCE_ID, DEBOUNCE_MS, nullptr);
            }
            return TRUE;

        case WM_DESTROY:
            KillTimer(hwnd, TIMER_POLL_ID);
            KillTimer(hwnd, TIMER_DEBOUNCE_ID);
            if (g_hDevNotifyHid) {
                UnregisterDeviceNotification(g_hDevNotifyHid);
                g_hDevNotifyHid = nullptr;
            }
            HidMonitor_Stop();
            Flyout_Shutdown();
            TaskbarBand_Shutdown();
            TrayIcons_Shutdown();
            PostQuitMessage(0);
            return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void PrintDiagLine(const wchar_t* s) {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!hOut || hOut == INVALID_HANDLE_VALUE) return;
    char buf[512] = {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, buf, sizeof(buf) - 2, nullptr, nullptr);
    if (n > 1) {
        buf[n - 1] = '\n';
        DWORD w = 0;
        WriteFile(hOut, buf, static_cast<DWORD>(n), &w, nullptr);
    }
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR cmdLine, int) {
    bool diag = (cmdLine && wcsstr(cmdLine, L"--diag") != nullptr);
    if (diag) {
        AttachConsole(ATTACH_PARENT_PROCESS);
    }

    g_singleMutex = CreateMutexW(nullptr, TRUE, L"Local\\TrayBattery_SingleInstance_v1");
    if (g_singleMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = FindWindowW(L"TrayBatteryHiddenHost", nullptr);
        if (existing) {
            PostMessageW(existing, WM_APP + 122, 0, 0);
        }
        CloseHandle(g_singleMutex);
        return 0;
    }

    EnsureAutoStartDefault();
    g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"TrayBatteryHiddenHost";
    RegisterClassExW(&wc);

    g_mainHwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW,
        L"TrayBatteryHiddenHost",
        L"TrayBattery",
        WS_POPUP,
        0, 0, 0, 0,
        nullptr, nullptr, hInst, nullptr);

    if (!g_mainHwnd) {
        if (diag) {
            wchar_t err[128] = {};
            swprintf_s(err, ARRAYSIZE(err), L"CreateWindow failed: err=%lu", GetLastError());
            PrintDiagLine(err);
        }
        return 1;
    }

    TrayIcons_Init(g_mainHwnd);
    TaskbarBand_Init(hInst, g_mainHwnd);

    FlyoutCallbacks fcb = {};
    fcb.onRefresh = OnRefresh;
    fcb.onToggleAutoStart = OnToggleAutoStart;
    fcb.onQuit = OnQuit;
    Flyout_Init(hInst, g_mainHwnd, fcb);

    HidMonitor_Start(g_mainHwnd);

    if (diag) {
        Sleep(300);
    }
    RefreshAll();

    if (diag) {
        wchar_t line[256] = {};
        swprintf_s(line, ARRAYSIZE(line), L"HWND=0x%p Devices=%d", g_mainHwnd, g_lastSnap.count);
        PrintDiagLine(line);
        for (int i = 0; i < g_lastSnap.count; ++i) {
            const DeviceInfo& d = g_lastSnap.items[i];
            swprintf_s(line, ARRAYSIZE(line), L"  [%d] %ls | type=%d | conn=%d | bat=%d%% | chg=%d | online=%d",
                       i, d.name, (int)d.type, (int)d.conn, d.battery, (int)d.charging, (int)d.connected);
            PrintDiagLine(line);
        }
        DestroyWindow(g_mainHwnd);
        if (g_singleMutex) CloseHandle(g_singleMutex);
        return 0;
    }

    MSG msg = {};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_singleMutex) CloseHandle(g_singleMutex);
    return static_cast<int>(msg.wParam);
}

extern "C" void wWinMainCRTStartup() {
    int code = wWinMain(GetModuleHandleW(nullptr), nullptr, GetCommandLineW(), SW_SHOWDEFAULT);
    ExitProcess(static_cast<UINT>(code));
}
