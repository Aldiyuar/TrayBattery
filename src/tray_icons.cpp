#include "tray_icons.h"
#include <shellapi.h>

struct TraySlot {
    bool active;
    wchar_t id[160];
    wchar_t tip[128];
    DeviceType type;
    int battery;
    bool charging;
    bool sleeping;
    HICON hIcon;
};

static HWND g_trayHwnd = nullptr;
static TraySlot g_slots[MAX_TRAY_ICONS] = {};

// 16x8 crisp silhouettes (each string has 16 chars: '#' = white, '.' = dark bg, ' ' = transparent corner)
static const char* ICON_MOUSE[8] = {
    "     ######     ",
    "    ##.##.##    ",
    "   ###.##.###   ",
    "   ##########   ",
    "   ##########   ",
    "   ##########   ",
    "    ########    ",
    "     ######     "
};

static const char* ICON_HEADPHONES[8] = {
    "    ########    ",
    "   ##      ##   ",
    "  ##        ##  ",
    " ###        ### ",
    " ####      #### ",
    " ####      #### ",
    "  ###      ###  ",
    "                "
};

static const char* ICON_GAMEPAD[8] = {
    "   ##########   ",
    "  ###.####.###  ",
    " ####......#### ",
    " ###.######.### ",
    " ####      #### ",
    " ###        ### ",
    "  #          #  ",
    "                "
};

static const char* ICON_KEYBOARD[8] = {
    " ############## ",
    " #.#.#.##.#.#.# ",
    " ############## ",
    " #.##.#.#.#.##. ",
    " ############## ",
    " ##.########.## ",
    " ############## ",
    "                "
};

static const char* ICON_SPEAKER[8] = {
    "    ########    ",
    "    ###..###    ",
    "    ########    ",
    "    ##....##    ",
    "    #......#    ",
    "    ##....##    ",
    "    ########    ",
    "                "
};

static const char* ICON_PHONE[8] = {
    "     ######     ",
    "     #....#     ",
    "     #....#     ",
    "     #....#     ",
    "     #....#     ",
    "     #....#     ",
    "     ##..##     ",
    "     ######     "
};

static const char* ICON_BT[8] = {
    "      ##        ",
    "      ###       ",
    "    # ## #      ",
    "     ####       ",
    "     ####       ",
    "    # ## #      ",
    "      ###       ",
    "      ##        "
};

// 4x6 pixel font for digits '0'..'9' and '-'
static const uint8_t GLYPH_DIGITS[11][6] = {
    { 0b0110, 0b1001, 0b1001, 0b1001, 0b1001, 0b0110 }, // 0
    { 0b0010, 0b0110, 0b0010, 0b0010, 0b0010, 0b0111 }, // 1
    { 0b0110, 0b1001, 0b0010, 0b0100, 0b1000, 0b1111 }, // 2
    { 0b1110, 0b0001, 0b0110, 0b0001, 0b0001, 0b1110 }, // 3
    { 0b1001, 0b1001, 0b1111, 0b0001, 0b0001, 0b0001 }, // 4
    { 0b1111, 0b1000, 0b1110, 0b0001, 0b0001, 0b1110 }, // 5
    { 0b0111, 0b1000, 0b1110, 0b1001, 0b1001, 0b0110 }, // 6
    { 0b1111, 0b0001, 0b0010, 0b0100, 0b0100, 0b0100 }, // 7
    { 0b0110, 0b1001, 0b0110, 0b1001, 0b1001, 0b0110 }, // 8
    { 0b0110, 0b1001, 0b0111, 0b0001, 0b0001, 0b1110 }, // 9
    { 0b0000, 0b0000, 0b1110, 0b0000, 0b0000, 0b0000 }  // 10 = '-'
};

// Narrow '1' (width 3) for "100"
static const uint8_t GLYPH_NARROW_ONE[6] = {
    0b010, 0b110, 0b010, 0b010, 0b010, 0b111
};

static inline uint32_t PremulARGB(uint8_t a, uint8_t r, uint8_t g, uint8_t b) {
    uint32_t pr = (uint32_t(r) * a) / 255;
    uint32_t pg = (uint32_t(g) * a) / 255;
    uint32_t pb = (uint32_t(b) * a) / 255;
    return (uint32_t(a) << 24) | (pr << 16) | (pg << 8) | pb;
}

static void DrawGlyph4x6(uint32_t canvas[16][16], int x0, int y0, int glyphIdx, uint32_t color) {
    for (int r = 0; r < 6; ++r) {
        uint8_t row = GLYPH_DIGITS[glyphIdx][r];
        for (int c = 0; c < 4; ++c) {
            if (row & (1 << (3 - c))) {
                int px = x0 + c;
                int py = y0 + r;
                if (px >= 0 && px < 16 && py >= 0 && py < 16) {
                    canvas[py][px] = color;
                }
            }
        }
    }
}

static void DrawNarrowOne(uint32_t canvas[16][16], int x0, int y0, uint32_t color) {
    for (int r = 0; r < 6; ++r) {
        uint8_t row = GLYPH_NARROW_ONE[r];
        for (int c = 0; c < 3; ++c) {
            if (row & (1 << (2 - c))) {
                int px = x0 + c;
                int py = y0 + r;
                if (px >= 0 && px < 16 && py >= 0 && py < 16) {
                    canvas[py][px] = color;
                }
            }
        }
    }
}

static HICON CreateDeviceTrayIcon(DeviceType type, int battery, bool charging, bool sleeping) {
    uint32_t grid[16][16] = {};

    // Top half (y=0..7): dark rounded backdrop for device silhouette (visible on both light & dark taskbars)
    const uint32_t topBg = PremulARGB(215, 22, 27, 34);
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 16; ++x) {
            if (y == 0 && (x == 0 || x == 15)) continue; // slight top corner rounding
            grid[y][x] = topBg;
        }
    }

    // Draw crisp white device silhouette in y=0..7
    const char** art = ICON_BT;
    switch (type) {
        case DeviceType::Mouse:      art = ICON_MOUSE; break;
        case DeviceType::Headphones: art = ICON_HEADPHONES; break;
        case DeviceType::Gamepad:    art = ICON_GAMEPAD; break;
        case DeviceType::Keyboard:   art = ICON_KEYBOARD; break;
        case DeviceType::Speaker:    art = ICON_SPEAKER; break;
        case DeviceType::Phone:      art = ICON_PHONE; break;
        default:                     art = ICON_BT; break;
    }
    const uint32_t fgWhite = sleeping ? PremulARGB(255, 190, 205, 225) : PremulARGB(255, 255, 255, 255);
    const uint32_t fgDark  = PremulARGB(255, 15, 23, 42);
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 16; ++x) {
            char ch = art[y][x];
            if (ch == '#') grid[y][x] = fgWhite;
            else if (ch == '.') grid[y][x] = fgDark;
        }
    }

    // Optional small yellow bolt corner when charging
    if (charging) {
        const uint32_t bolt = PremulARGB(255, 250, 204, 21);
        grid[1][14] = bolt;
        grid[2][13] = bolt;
        grid[3][13] = bolt; grid[3][14] = bolt;
        grid[4][14] = bolt;
        grid[5][13] = bolt;
    }

    // Bottom half (y=8..15): color-coded battery badge
    uint8_t br = 71, bg = 85, bb = 105; // default slate gray
    if (charging) {
        br = 2; bg = 132; bb = 199;     // sky blue
    } else if (sleeping && battery >= 0) {
        br = 71; bg = 85; bb = 105;     // muted slate when sleeping
    } else if (battery >= 50) {
        br = 22; bg = 163; bb = 74;     // green
    } else if (battery >= 21) {
        br = 217; bg = 119; bb = 6;     // amber
    } else if (battery >= 0) {
        br = 220; bg = 38; bb = 38;     // red
    }

    const uint32_t badgeColor = PremulARGB(255, br, bg, bb);
    for (int y = 8; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            if (y == 15 && (x == 0 || x == 15)) continue; // slight bottom corner rounding
            grid[y][x] = badgeColor;
        }
    }

    // Render percentage digits centered in y=9..14
    const uint32_t txtColor = PremulARGB(255, 255, 255, 255);
    if (battery < 0) {
        DrawGlyph4x6(grid, 3, 9, 10, txtColor);
        DrawGlyph4x6(grid, 9, 9, 10, txtColor);
    } else if (battery >= 100) {
        DrawNarrowOne(grid, 2, 9, txtColor);
        DrawGlyph4x6(grid, 6, 9, 0, txtColor);
        DrawGlyph4x6(grid, 11, 9, 0, txtColor);
    } else if (battery >= 10) {
        DrawGlyph4x6(grid, 3, 9, battery / 10, txtColor);
        DrawGlyph4x6(grid, 9, 9, battery % 10, txtColor);
    } else {
        DrawGlyph4x6(grid, 6, 9, battery, txtColor);
    }

    // Create target icon sized to system small icon metric (16x16 at 100% DPI, scaled cleanly at higher DPI)
    int iconSz = GetSystemMetrics(SM_CXSMICON);
    if (iconSz < 16) iconSz = 16;

    BITMAPV5HEADER bi = {};
    bi.bV5Size = sizeof(BITMAPV5HEADER);
    bi.bV5Width = iconSz;
    bi.bV5Height = -iconSz; // top-down
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask   = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask  = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;

    void* pvBits = nullptr;
    HDC hdc = GetDC(nullptr);
    HBITMAP hColor = CreateDIBSection(hdc, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &pvBits, nullptr, 0);
    ReleaseDC(nullptr, hdc);
    if (!hColor || !pvBits) {
        if (hColor) DeleteObject(hColor);
        return nullptr;
    }

    uint32_t* dst = static_cast<uint32_t*>(pvBits);
    for (int y = 0; y < iconSz; ++y) {
        int sy = (y * 16) / iconSz;
        for (int x = 0; x < iconSz; ++x) {
            int sx = (x * 16) / iconSz;
            dst[y * iconSz + x] = grid[sy][sx];
        }
    }

    HBITMAP hMask = CreateBitmap(iconSz, iconSz, 1, 1, nullptr);
    ICONINFO ii = {};
    ii.fIcon = TRUE;
    ii.hbmMask = hMask;
    ii.hbmColor = hColor;
    HICON hIcon = CreateIconIndirect(&ii);

    DeleteObject(hMask);
    DeleteObject(hColor);
    return hIcon;
}

static void BuildTooltip(const DeviceInfo* dev, int totalConnected, wchar_t* outTip, size_t outCount) {
    if (!dev) {
        SafeCopyW(outTip, outCount, L"TrayBattery — нет подключённых устройств");
        return;
    }
    wchar_t batStr[32] = {};
    if (dev->battery < 0) {
        SafeCopyW(batStr, ARRAYSIZE(batStr), L"ожидание...");
    } else if (dev->charging) {
        swprintf_s(batStr, ARRAYSIZE(batStr), L"%d%% (зарядка)", dev->battery);
    } else if (dev->sleeping) {
        swprintf_s(batStr, ARRAYSIZE(batStr), L"%d%% (спит)", dev->battery);
    } else {
        swprintf_s(batStr, ARRAYSIZE(batStr), L"%d%%", dev->battery);
    }

    if (totalConnected > MAX_TRAY_ICONS) {
        swprintf_s(outTip, outCount, L"%ls: %ls\n(всего устройств: %d, клик — список)",
                   dev->name, batStr, totalConnected);
    } else {
        swprintf_s(outTip, outCount, L"%ls: %ls (%ls)",
                   dev->name, batStr, ConnectionKindLabel(dev->conn));
    }
}

void TrayIcons_Init(HWND hwnd) {
    g_trayHwnd = hwnd;
    ZeroMemory(g_slots, sizeof(g_slots));
}

void TrayIcons_Update(const DeviceSnapshot* snapshot) {
    if (!g_trayHwnd) return;

    // Devices 0..4 go to the open taskbar band; devices 5+ overflow into the system tray!
    const DeviceInfo* overflowDevs[MAX_TRAY_ICONS] = {};
    int overflowCount = 0;
    int totalConnected = 0;

    if (snapshot) {
        for (int i = 0; i < snapshot->count; ++i) {
            if (snapshot->items[i].connected) {
                if (totalConnected >= MAX_TRAY_ICONS && overflowCount < MAX_TRAY_ICONS) {
                    overflowDevs[overflowCount++] = &snapshot->items[i];
                }
                totalConnected++;
            }
        }
    }

    // If 0 devices connected, show 1 placeholder icon in tray so user can still access the window/menu.
    // If 1..5 devices connected, all are shown on the open taskbar band (0 extra tray icons).
    int targetSlots = (totalConnected == 0) ? 1 : overflowCount;

    for (int i = 0; i < MAX_TRAY_ICONS; ++i) {
        TraySlot& slot = g_slots[i];

        if (i >= targetSlots) {
            if (slot.active) {
                NOTIFYICONDATAW nid = {};
                nid.cbSize = sizeof(nid);
                nid.hWnd = g_trayHwnd;
                nid.uID = static_cast<UINT>(i + 1);
                Shell_NotifyIconW(NIM_DELETE, &nid);
                if (slot.hIcon) {
                    DestroyIcon(slot.hIcon);
                    slot.hIcon = nullptr;
                }
                ZeroMemory(&slot, sizeof(slot));
            }
            continue;
        }

        const DeviceInfo* dev = (overflowCount > 0) ? overflowDevs[i] : nullptr;
        const wchar_t* newId = dev ? dev->id : L"__EMPTY__";
        DeviceType newType = dev ? dev->type : DeviceType::Unknown;
        int newBat = dev ? dev->battery : -1;
        bool newChg = dev ? dev->charging : false;
        bool newSlp = dev ? dev->sleeping : false;

        wchar_t newTip[128] = {};
        BuildTooltip(dev, totalConnected, newTip, ARRAYSIZE(newTip));

        bool iconChanged = (!slot.active || slot.type != newType || slot.battery != newBat ||
                            slot.charging != newChg || slot.sleeping != newSlp || !slot.hIcon);
        bool tipChanged  = (!slot.active || wcscmp(slot.tip, newTip) != 0);
        bool idChanged   = (!slot.active || wcscmp(slot.id, newId) != 0);

        if (!iconChanged && !tipChanged && !idChanged) {
            continue; // Zero GDI / Shell overhead when nothing changed!
        }

        HICON oldIcon = slot.hIcon;
        if (iconChanged) {
            slot.hIcon = CreateDeviceTrayIcon(newType, newBat, newChg, newSlp);
        }

        NOTIFYICONDATAW nid = {};
        nid.cbSize = sizeof(nid);
        nid.hWnd = g_trayHwnd;
        nid.uID = static_cast<UINT>(i + 1);
        nid.uCallbackMessage = WM_APP_TRAY_BASE + static_cast<UINT>(i);
        nid.hIcon = slot.hIcon;
        SafeCopyW(nid.szTip, ARRAYSIZE(nid.szTip), newTip);
        nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;

        BOOL ok = Shell_NotifyIconW(slot.active ? NIM_MODIFY : NIM_ADD, &nid);
        if (!ok && slot.active) {
            // Re-add if taskbar recreated
            ok = Shell_NotifyIconW(NIM_ADD, &nid);
        }

        if (iconChanged && oldIcon) {
            DestroyIcon(oldIcon);
        }

        slot.active = true;
        SafeCopyW(slot.id, ARRAYSIZE(slot.id), newId);
        SafeCopyW(slot.tip, ARRAYSIZE(slot.tip), newTip);
        slot.type = newType;
        slot.battery = newBat;
        slot.charging = newChg;
        slot.sleeping = newSlp;
    }
}

void TrayIcons_ReAddAll() {
    if (!g_trayHwnd) return;
    for (int i = 0; i < MAX_TRAY_ICONS; ++i) {
        TraySlot& slot = g_slots[i];
        if (!slot.active || !slot.hIcon) continue;
        NOTIFYICONDATAW nid = {};
        nid.cbSize = sizeof(nid);
        nid.hWnd = g_trayHwnd;
        nid.uID = static_cast<UINT>(i + 1);
        nid.uCallbackMessage = WM_APP_TRAY_BASE + static_cast<UINT>(i);
        nid.hIcon = slot.hIcon;
        SafeCopyW(nid.szTip, ARRAYSIZE(nid.szTip), slot.tip);
        nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        Shell_NotifyIconW(NIM_ADD, &nid);
    }
}

void TrayIcons_Shutdown() {
    if (!g_trayHwnd) return;
    for (int i = 0; i < MAX_TRAY_ICONS; ++i) {
        TraySlot& slot = g_slots[i];
        if (slot.active) {
            NOTIFYICONDATAW nid = {};
            nid.cbSize = sizeof(nid);
            nid.hWnd = g_trayHwnd;
            nid.uID = static_cast<UINT>(i + 1);
            Shell_NotifyIconW(NIM_DELETE, &nid);
        }
        if (slot.hIcon) {
            DestroyIcon(slot.hIcon);
            slot.hIcon = nullptr;
        }
        slot.active = false;
    }
}

int TrayIcons_VisibleLimit() {
    return MAX_TRAY_ICONS;
}

HICON TrayIcons_CreateAppIcon() {
    return CreateDeviceTrayIcon(DeviceType::Mouse, 80, false, false);
}
