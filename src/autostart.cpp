#include "autostart.h"

static const wchar_t* RUN_REG_PATH = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* APP_REG_PATH = L"Software\\TrayBattery";
static const wchar_t* RUN_VALUE_NAME = L"TrayBattery";

static bool GetQuotedExePath(wchar_t* outBuf, DWORD outCount) {
    wchar_t exePath[MAX_PATH] = {};
    DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (len == 0 || len >= MAX_PATH - 4) return false;
    swprintf_s(outBuf, outCount, L"\"%ls\"", exePath);
    return true;
}

bool IsAutoStartEnabled() {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_REG_PATH, 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        return false;
    }
    wchar_t val[MAX_PATH + 16] = {};
    DWORD sz = sizeof(val);
    DWORD type = 0;
    LSTATUS st = RegQueryValueExW(hKey, RUN_VALUE_NAME, nullptr, &type, reinterpret_cast<BYTE*>(val), &sz);
    RegCloseKey(hKey);
    return (st == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ) && val[0] != L'\0');
}

bool SetAutoStartEnabled(bool enable) {
    HKEY hAppKey = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, APP_REG_PATH, 0, nullptr, 0, KEY_WRITE, nullptr, &hAppKey, nullptr) == ERROR_SUCCESS) {
        DWORD one = 1;
        RegSetValueExW(hAppKey, L"AutoStartConfigured", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&one), sizeof(one));
        RegCloseKey(hAppKey);
    }

    HKEY hKey = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, RUN_REG_PATH, 0, nullptr, 0, KEY_WRITE, nullptr, &hKey, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    bool ok = false;
    if (enable) {
        wchar_t quoted[MAX_PATH + 8] = {};
        if (GetQuotedExePath(quoted, ARRAYSIZE(quoted))) {
            DWORD bytes = static_cast<DWORD>((wcslen(quoted) + 1) * sizeof(wchar_t));
            ok = (RegSetValueExW(hKey, RUN_VALUE_NAME, 0, REG_SZ, reinterpret_cast<const BYTE*>(quoted), bytes) == ERROR_SUCCESS);
        }
    } else {
        LSTATUS st = RegDeleteValueW(hKey, RUN_VALUE_NAME);
        ok = (st == ERROR_SUCCESS || st == ERROR_FILE_NOT_FOUND);
    }
    RegCloseKey(hKey);
    return ok;
}

void EnsureAutoStartDefault() {
    HKEY hAppKey = nullptr;
    DWORD configured = 0;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, APP_REG_PATH, 0, KEY_READ, &hAppKey) == ERROR_SUCCESS) {
        DWORD sz = sizeof(configured);
        DWORD type = 0;
        if (RegQueryValueExW(hAppKey, L"AutoStartConfigured", nullptr, &type, reinterpret_cast<BYTE*>(&configured), &sz) != ERROR_SUCCESS) {
            configured = 0;
        }
        RegCloseKey(hAppKey);
    }

    if (configured == 0) {
        SetAutoStartEnabled(true);
    } else if (IsAutoStartEnabled()) {
        // Keep path up to date if executable moved
        SetAutoStartEnabled(true);
    }
}
