#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <stdint.h>
#include <wchar.h>

constexpr int MAX_TRAY_ICONS = 5;
constexpr int MAX_DEVICES = 32;

constexpr UINT WM_APP_TRAY_BASE  = WM_APP + 100; // + slot (0..4)
constexpr UINT WM_APP_HID_UPDATE = WM_APP + 120;
constexpr UINT_PTR TIMER_POLL_ID = 1;
constexpr UINT POLL_INTERVAL_MS  = 60000; // 60 seconds

enum class DeviceType : uint8_t {
    Unknown = 0,
    Mouse,
    Headphones,
    Gamepad,
    Keyboard,
    Speaker,
    Phone
};

enum class ConnectionKind : uint8_t {
    Bluetooth = 0,
    BluetoothLE,
    Dongle24G,
    BluetoothHID
};

struct DeviceInfo {
    wchar_t id[160];        // Stable unique ID (ContainerId / MAC / HID path key)
    wchar_t name[128];      // Display name
    DeviceType type;
    ConnectionKind conn;
    int battery;            // -1 = unknown / waiting, 0..100 = percent
    bool charging;
    bool connected;
    bool sleeping;
    ULONGLONG lastUpdateTick;
};

struct DeviceSnapshot {
    int count;
    DeviceInfo items[MAX_DEVICES];
};

inline void SafeCopyW(wchar_t* dst, size_t dstCount, const wchar_t* src) {
    if (!dst || dstCount == 0) return;
    if (!src) { dst[0] = L'\0'; return; }
    size_t i = 0;
    for (; i + 1 < dstCount && src[i] != L'\0'; ++i) {
        dst[i] = src[i];
    }
    dst[i] = L'\0';
}

inline const wchar_t* ConnectionKindLabel(ConnectionKind k) {
    switch (k) {
        case ConnectionKind::Dongle24G:    return L"2.4 ГГц донгл";
        case ConnectionKind::BluetoothLE:  return L"Bluetooth LE";
        case ConnectionKind::BluetoothHID: return L"Bluetooth HID";
        case ConnectionKind::Bluetooth:
        default:                           return L"Bluetooth";
    }
}

inline const wchar_t* DeviceTypeLabel(DeviceType t) {
    switch (t) {
        case DeviceType::Mouse:      return L"Мышь";
        case DeviceType::Headphones: return L"Наушники";
        case DeviceType::Gamepad:    return L"Геймпад";
        case DeviceType::Keyboard:   return L"Клавиатура";
        case DeviceType::Speaker:    return L"Колонка";
        case DeviceType::Phone:      return L"Телефон";
        default:                     return L"Устройство";
    }
}
