#include "hid_monitor.h"
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>

struct HidDeviceSlot {
    bool active;
    bool isCompX;
    bool isPowerDevice;
    wchar_t devPath[MAX_PATH * 2];
    wchar_t id[160];
    wchar_t name[128];
    DeviceType type;
    ConnectionKind conn;
    int battery;
    bool charging;
    bool sleeping;
    ULONGLONG lastPacketTick;

    HANDLE hFile;
    OVERLAPPED ov;
    bool ioPending;
    BYTE readBuf[64];
    DWORD reportLen;
};

static CRITICAL_SECTION g_hidCs;
static bool g_csInit = false;
static HWND g_notifyWnd = nullptr;
static HANDLE g_thread = nullptr;
static HANDLE g_stopEvent = nullptr;
static HANDLE g_rescanEvent = nullptr;
static HidDeviceSlot g_slots[8] = {};
static int g_slotCount = 0;
static unsigned g_nextHidUid = 1;

static void CloseSlotHandle(HidDeviceSlot* s) {
    if (s->ioPending && s->hFile != INVALID_HANDLE_VALUE) {
        CancelIoEx(s->hFile, &s->ov);
        DWORD transferred = 0;
        GetOverlappedResult(s->hFile, &s->ov, &transferred, TRUE);
    }
    s->ioPending = false;
    if (s->hFile != INVALID_HANDLE_VALUE) {
        CloseHandle(s->hFile);
        s->hFile = INVALID_HANDLE_VALUE;
    }
    if (s->ov.hEvent) {
        CloseHandle(s->ov.hEvent);
        s->ov.hEvent = nullptr;
    }
}

static void CloseAllSlots() {
    for (int i = 0; i < g_slotCount; ++i) {
        CloseSlotHandle(&g_slots[i]);
    }
    g_slotCount = 0;
}

static bool IsSamePath(const wchar_t* a, const wchar_t* b) {
    return _wcsicmp(a, b) == 0;
}

static void FriendlyHidName(HANDLE h, USHORT vid, USHORT pid, wchar_t* outName, size_t outCount) {
    if (vid == 0x1D57 && pid == 0xFA60) {
        SafeCopyW(outName, outCount, L"Attack Shark R1");
        return;
    }
    wchar_t prod[128] = {};
    if (HidD_GetProductString(h, prod, sizeof(prod)) && prod[0] != L'\0') {
        SafeCopyW(outName, outCount, prod);
        return;
    }
    swprintf_s(outName, outCount, L"HID %04X:%04X", vid, pid);
}

static void ScanHidDevices() {
    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);

    HDEVINFO hDevInfo = SetupDiGetClassDevsW(&hidGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (hDevInfo == INVALID_HANDLE_VALUE) return;

    bool keep[8] = {};
    SP_DEVICE_INTERFACE_DATA ifData = {};
    ifData.cbSize = sizeof(ifData);

    for (DWORD idx = 0; SetupDiEnumDeviceInterfaces(hDevInfo, nullptr, &hidGuid, idx, &ifData); ++idx) {
        DWORD reqSize = 0;
        SetupDiGetDeviceInterfaceDetailW(hDevInfo, &ifData, nullptr, 0, &reqSize, nullptr);
        if (reqSize == 0 || reqSize > 2048) continue;

        BYTE detailBuf[2048] = {};
        auto* detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(detailBuf);
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(hDevInfo, &ifData, detail, reqSize, nullptr, nullptr)) continue;

        const wchar_t* path = detail->DevicePath;

        // Check if already tracked
        int existingIdx = -1;
        for (int i = 0; i < g_slotCount; ++i) {
            if (IsSamePath(g_slots[i].devPath, path)) {
                existingIdx = i;
                break;
            }
        }
        if (existingIdx >= 0) {
            keep[existingIdx] = true;
            continue;
        }

        // Open query handle (no access rights needed for attributes/caps)
        HANDLE hQ = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (hQ == INVALID_HANDLE_VALUE) continue;

        HIDD_ATTRIBUTES attrs = {};
        attrs.Size = sizeof(attrs);
        bool gotAttrs = HidD_GetAttributes(hQ, &attrs);

        PHIDP_PREPARSED_DATA ppd = nullptr;
        HIDP_CAPS caps = {};
        bool gotCaps = false;
        if (HidD_GetPreparsedData(hQ, &ppd)) {
            gotCaps = (HidP_GetCaps(ppd, &caps) == HIDP_STATUS_SUCCESS);
            HidD_FreePreparsedData(ppd);
        }

        if (!gotAttrs || !gotCaps) {
            CloseHandle(hQ);
            continue;
        }

        bool isCompXBattery = ((attrs.VendorID == 0x1D57 || attrs.VendorID == 0x3554) &&
                               caps.UsagePage == 0x000A && caps.Usage == 0x0000 &&
                               caps.InputReportByteLength == 5);

        if (!isCompXBattery) {
            CloseHandle(hQ);
            continue;
        }

        wchar_t dispName[128] = {};
        FriendlyHidName(hQ, attrs.VendorID, attrs.ProductID, dispName, ARRAYSIZE(dispName));
        CloseHandle(hQ);

        if (g_slotCount >= 8) continue;

        HANDLE hRead = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (hRead == INVALID_HANDLE_VALUE) continue;

        HidDeviceSlot& s = g_slots[g_slotCount];
        ZeroMemory(&s, sizeof(s));
        s.active = true;
        s.isCompX = true;
        SafeCopyW(s.devPath, ARRAYSIZE(s.devPath), path);
        swprintf_s(s.id, ARRAYSIZE(s.id), L"HID_%04X_%04X_%u", attrs.VendorID, attrs.ProductID, g_nextHidUid++);
        SafeCopyW(s.name, ARRAYSIZE(s.name), dispName);
        s.type = DeviceType::Mouse;
        s.conn = ConnectionKind::Dongle24G;
        s.battery = -1;
        s.charging = false;
        s.sleeping = false;
        s.lastPacketTick = 0;
        s.hFile = hRead;
        s.ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        s.reportLen = caps.InputReportByteLength;
        s.ioPending = false;

        keep[g_slotCount] = true;
        g_slotCount++;
    }

    SetupDiDestroyDeviceInfoList(hDevInfo);

    // Compact removed slots safely: cancel pending OVERLAPPED I/O before moving any slot in memory
    int writeIdx = 0;
    for (int i = 0; i < g_slotCount; ++i) {
        if (!keep[i]) {
            CloseSlotHandle(&g_slots[i]);
        } else {
            if (writeIdx != i) {
                if (g_slots[i].ioPending && g_slots[i].hFile != INVALID_HANDLE_VALUE) {
                    CancelIoEx(g_slots[i].hFile, &g_slots[i].ov);
                    DWORD transferred = 0;
                    GetOverlappedResult(g_slots[i].hFile, &g_slots[i].ov, &transferred, TRUE);
                    g_slots[i].ioPending = false;
                }
                g_slots[writeIdx] = g_slots[i];
                ZeroMemory(&g_slots[i], sizeof(g_slots[i]));
                g_slots[i].hFile = INVALID_HANDLE_VALUE;
            }
            writeIdx++;
        }
    }
    g_slotCount = writeIdx;
}

static void ArmPendingReads() {
    for (int i = 0; i < g_slotCount; ++i) {
        HidDeviceSlot& s = g_slots[i];
        if (!s.ioPending && s.hFile != INVALID_HANDLE_VALUE && s.ov.hEvent) {
            ResetEvent(s.ov.hEvent);
            DWORD bytesRead = 0;
            BOOL ok = ReadFile(s.hFile, s.readBuf, s.reportLen, &bytesRead, &s.ov);
            if (ok) {
                SetEvent(s.ov.hEvent);
                s.ioPending = true;
            } else if (GetLastError() == ERROR_IO_PENDING) {
                s.ioPending = true;
            } else {
                // Device probably disconnected
                CloseSlotHandle(&s);
                s.active = false;
            }
        }
    }
}

static bool ProcessCompletedRead(HidDeviceSlot& s) {
    DWORD bytesRead = 0;
    if (!GetOverlappedResult(s.hFile, &s.ov, &bytesRead, FALSE)) {
        DWORD err = GetLastError();
        s.ioPending = false;
        if (err == ERROR_DEVICE_NOT_CONNECTED || err == ERROR_GEN_FAILURE || err == ERROR_OPERATION_ABORTED) {
            CloseSlotHandle(&s);
            s.active = false;
        }
        return false;
    }
    s.ioPending = false;

    bool changed = false;
    if (s.isCompX && bytesRead >= 5 && s.readBuf[0] == 0x03) {
        uint16_t code = static_cast<uint16_t>(s.readBuf[1]) | (static_cast<uint16_t>(s.readBuf[2]) << 8);
        if (code == 0x4010) {
            bool chg = (s.readBuf[3] == 0x00);
            int lvl = static_cast<int>(s.readBuf[4]);
            if (lvl >= 1 && lvl <= 10) {
                int pct = lvl * 10;
                if (s.battery != pct || s.charging != chg || s.sleeping) {
                    changed = true;
                }
                s.battery = pct;
                s.charging = chg;
                s.sleeping = false;
                s.lastPacketTick = GetTickCount64();
            }
        }
    }
    return changed;
}

static DWORD WINAPI HidThreadProc(LPVOID) {
    EnterCriticalSection(&g_hidCs);
    ScanHidDevices();
    ArmPendingReads();
    LeaveCriticalSection(&g_hidCs);

    if (g_notifyWnd) {
        PostMessageW(g_notifyWnd, WM_APP_HID_UPDATE, 0, 0);
    }

    for (;;) {
        HANDLE waitHandles[10] = { g_stopEvent, g_rescanEvent };
        int slotMap[10] = { -1, -1 };
        DWORD count = 2;

        EnterCriticalSection(&g_hidCs);
        for (int i = 0; i < g_slotCount && count < 10; ++i) {
            if (g_slots[i].ioPending && g_slots[i].ov.hEvent) {
                waitHandles[count] = g_slots[i].ov.hEvent;
                slotMap[count] = i;
                count++;
            }
        }
        LeaveCriticalSection(&g_hidCs);

        // Wake every 120s only to check if mouse went to sleep (> 3 min without packets)
        DWORD waitMs = (count > 2) ? 120000 : INFINITE;
        DWORD res = WaitForMultipleObjects(count, waitHandles, FALSE, waitMs);

        if (res == WAIT_OBJECT_0) {
            break; // stopEvent
        }

        bool notify = false;

        if (res == WAIT_OBJECT_0 + 1) {
            // rescanEvent
            EnterCriticalSection(&g_hidCs);
            int prevCount = g_slotCount;
            ScanHidDevices();
            ArmPendingReads();
            if (g_slotCount != prevCount) notify = true;
            LeaveCriticalSection(&g_hidCs);
        } else if (res >= WAIT_OBJECT_0 + 2 && res < WAIT_OBJECT_0 + count) {
            int idx = slotMap[res - WAIT_OBJECT_0];
            EnterCriticalSection(&g_hidCs);
            if (idx >= 0 && idx < g_slotCount && g_slots[idx].ioPending) {
                if (ProcessCompletedRead(g_slots[idx])) {
                    notify = true;
                }
                if (!g_slots[idx].active) {
                    ScanHidDevices();
                    notify = true;
                }
                ArmPendingReads();
            }
            LeaveCriticalSection(&g_hidCs);
        } else if (res == WAIT_TIMEOUT) {
            ULONGLONG now = GetTickCount64();
            EnterCriticalSection(&g_hidCs);
            for (int i = 0; i < g_slotCount; ++i) {
                if (g_slots[i].battery >= 0 && !g_slots[i].sleeping &&
                    g_slots[i].lastPacketTick > 0 && (now - g_slots[i].lastPacketTick) > 180000ULL) {
                    g_slots[i].sleeping = true;
                    notify = true;
                }
            }
            LeaveCriticalSection(&g_hidCs);
        }

        if (notify && g_notifyWnd) {
            PostMessageW(g_notifyWnd, WM_APP_HID_UPDATE, 0, 0);
        }
    }

    EnterCriticalSection(&g_hidCs);
    CloseAllSlots();
    LeaveCriticalSection(&g_hidCs);
    return 0;
}

void HidMonitor_Start(HWND notifyWnd) {
    if (!g_csInit) {
        InitializeCriticalSection(&g_hidCs);
        g_csInit = true;
    }
    g_notifyWnd = notifyWnd;
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_rescanEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_thread = CreateThread(nullptr, 0, HidThreadProc, nullptr, 0, nullptr);
}

void HidMonitor_Stop() {
    if (g_stopEvent) SetEvent(g_stopEvent);
    if (g_thread) {
        WaitForSingleObject(g_thread, 3000);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
    if (g_stopEvent) { CloseHandle(g_stopEvent); g_stopEvent = nullptr; }
    if (g_rescanEvent) { CloseHandle(g_rescanEvent); g_rescanEvent = nullptr; }
    if (g_csInit) {
        DeleteCriticalSection(&g_hidCs);
        g_csInit = false;
    }
}

void HidMonitor_TriggerRescan() {
    if (g_rescanEvent) SetEvent(g_rescanEvent);
}

void HidMonitor_CollectDevices(DeviceSnapshot* outSnapshot) {
    if (!outSnapshot || !g_csInit) return;
    EnterCriticalSection(&g_hidCs);
    for (int i = 0; i < g_slotCount && outSnapshot->count < MAX_DEVICES; ++i) {
        const HidDeviceSlot& s = g_slots[i];
        if (!s.active) continue;
        DeviceInfo& dst = outSnapshot->items[outSnapshot->count++];
        ZeroMemory(&dst, sizeof(dst));
        SafeCopyW(dst.id, ARRAYSIZE(dst.id), s.id);
        SafeCopyW(dst.name, ARRAYSIZE(dst.name), s.name);
        dst.type = s.type;
        dst.conn = s.conn;
        dst.battery = s.battery;
        dst.charging = s.charging;
        dst.connected = true;
        dst.sleeping = s.sleeping;
        dst.lastUpdateTick = s.lastPacketTick;
    }
    LeaveCriticalSection(&g_hidCs);
}
