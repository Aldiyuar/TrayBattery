#include "bt_monitor.h"
#include <initguid.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <devpkey.h>
#include <bluetoothapis.h>
#include <bluetoothleapis.h>
#include <bthledef.h>

// {83DA6326-97A6-4088-9453-A1923F573B29}, 15 — DEVPKEY_Device_IsConnected
static const DEVPROPKEY PKEY_DEV_IS_CONNECTED = {
    { 0x83DA6326, 0x97A6, 0x4088, { 0x94, 0x53, 0xA1, 0x92, 0x3F, 0x57, 0x3B, 0x29 } }, 15
};

// {104EA319-6EE2-4701-BD47-8DDBF425BBE5}, 2 — DEVPKEY_Bluetooth_Battery
static const DEVPROPKEY PKEY_BT_BATTERY = {
    { 0x104EA319, 0x6EE2, 0x4701, { 0xBD, 0x47, 0x8D, 0xDB, 0xF4, 0x25, 0xBB, 0xE5 } }, 2
};

// {2BD67D8B-8BEB-48D5-87E0-6CDA3428040A}, 4 — DEVPKEY_Bluetooth_ClassOfDevice
static const DEVPROPKEY PKEY_BT_COD = {
    { 0x2BD67D8B, 0x8BEB, 0x48D5, { 0x87, 0xE0, 0x6C, 0xDA, 0x34, 0x28, 0x04, 0x0A } }, 4
};

// {2BD67D8B-8BEB-48D5-87E0-6CDA3428040A}, 1 — DEVPKEY_Bluetooth_DeviceAddress
static const DEVPROPKEY PKEY_BT_ADDR = {
    { 0x2BD67D8B, 0x8BEB, 0x48D5, { 0x87, 0xE0, 0x6C, 0xDA, 0x34, 0x28, 0x04, 0x0A } }, 1
};

// {0000180F-0000-1000-8000-00805F9B34FB} — BLE Battery Service UUID
static const GUID GUID_BLE_BATTERY_SERVICE = {
    0x0000180F, 0x0000, 0x1000, { 0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB }
};

struct BtContainerEntry {
    bool used;
    GUID containerId;
    bool hasContainer;
    wchar_t macHex[16];
    wchar_t name[128];
    DWORD cod;
    int battery;
    bool connected;
    bool isLE;
};

static bool ContainsInsensitive(const wchar_t* hay, const wchar_t* needle) {
    if (!hay || !needle || !*needle) return false;
    size_t nlen = wcslen(needle);
    for (const wchar_t* p = hay; *p; ++p) {
        if (_wcsnicmp(p, needle, nlen) == 0) return true;
    }
    return false;
}

static DeviceType ClassifyBluetoothDevice(DWORD cod, const wchar_t* name) {
    if (cod != 0) {
        DWORD major = (cod >> 8) & 0x1F;
        DWORD minor = (cod >> 2) & 0x3F;
        if (major == 0x05) { // Peripheral
            if ((minor & 0x30) == 0x20 || (cod & 0xC0) == 0x80) return DeviceType::Mouse;
            if ((minor & 0x30) == 0x10 || (cod & 0xC0) == 0x40) return DeviceType::Keyboard;
            if ((minor & 0x0F) == 0x01 || (minor & 0x0F) == 0x02) return DeviceType::Gamepad;
        } else if (major == 0x04) { // Audio/Video
            if (minor == 0x05) return DeviceType::Speaker;
            return DeviceType::Headphones;
        } else if (major == 0x02) { // Phone
            return DeviceType::Phone;
        }
    }

    if (name && name[0]) {
        if (ContainsInsensitive(name, L"mouse") || ContainsInsensitive(name, L"мышь") ||
            ContainsInsensitive(name, L"shark") || ContainsInsensitive(name, L"mx master") ||
            ContainsInsensitive(name, L"mx anywhere") || ContainsInsensitive(name, L"viper") ||
            ContainsInsensitive(name, L"deathadder") || ContainsInsensitive(name, L"superlight")) {
            return DeviceType::Mouse;
        }
        if (ContainsInsensitive(name, L"controller") || ContainsInsensitive(name, L"gamepad") ||
            ContainsInsensitive(name, L"xbox") || ContainsInsensitive(name, L"dualsense") ||
            ContainsInsensitive(name, L"dualshock") || ContainsInsensitive(name, L"joy-con") ||
            ContainsInsensitive(name, L"8bitdo") || ContainsInsensitive(name, L"геймпад")) {
            return DeviceType::Gamepad;
        }
        if (ContainsInsensitive(name, L"keyboard") || ContainsInsensitive(name, L"keychron") ||
            ContainsInsensitive(name, L"клавиатур") || ContainsInsensitive(name, L"k380") ||
            ContainsInsensitive(name, L"nuphy")) {
            return DeviceType::Keyboard;
        }
        if (ContainsInsensitive(name, L"buds") || ContainsInsensitive(name, L"pods") ||
            ContainsInsensitive(name, L"head") || ContainsInsensitive(name, L"ear") ||
            ContainsInsensitive(name, L"wh-") || ContainsInsensitive(name, L"wf-") ||
            ContainsInsensitive(name, L"freebuds") || ContainsInsensitive(name, L"airpods") ||
            ContainsInsensitive(name, L"наушн") || ContainsInsensitive(name, L"гарнит")) {
            return DeviceType::Headphones;
        }
        if (ContainsInsensitive(name, L"speaker") || ContainsInsensitive(name, L"колонк") ||
            ContainsInsensitive(name, L"jbl ") || ContainsInsensitive(name, L"marshall")) {
            return DeviceType::Speaker;
        }
        if (ContainsInsensitive(name, L"iphone") || ContainsInsensitive(name, L"galaxy") ||
            ContainsInsensitive(name, L"pixel") || ContainsInsensitive(name, L"phone")) {
            return DeviceType::Phone;
        }
    }
    return DeviceType::Unknown;
}

static bool ReadDevPropGuid(DEVINST devInst, const DEVPROPKEY* key, GUID* outGuid) {
    DEVPROPTYPE type = 0;
    ULONG size = sizeof(GUID);
    return (CM_Get_DevNode_PropertyW(devInst, key, &type, reinterpret_cast<PBYTE>(outGuid), &size, 0) == CR_SUCCESS &&
            type == DEVPROP_TYPE_GUID);
}

static bool ReadDevPropBool(DEVINST devInst, const DEVPROPKEY* key, bool* outVal) {
    DEVPROPTYPE type = 0;
    DEVPROP_BOOLEAN val = DEVPROP_FALSE;
    ULONG size = sizeof(val);
    if (CM_Get_DevNode_PropertyW(devInst, key, &type, reinterpret_cast<PBYTE>(&val), &size, 0) == CR_SUCCESS &&
        type == DEVPROP_TYPE_BOOLEAN) {
        *outVal = (val != DEVPROP_FALSE);
        return true;
    }
    return false;
}

static bool ReadDevPropUint32(DEVINST devInst, const DEVPROPKEY* key, DWORD* outVal) {
    DEVPROPTYPE type = 0;
    UINT32 val = 0;
    ULONG size = sizeof(val);
    if (CM_Get_DevNode_PropertyW(devInst, key, &type, reinterpret_cast<PBYTE>(&val), &size, 0) == CR_SUCCESS &&
        (type == DEVPROP_TYPE_UINT32 || type == DEVPROP_TYPE_INT32)) {
        *outVal = val;
        return true;
    }
    return false;
}

static bool ReadDevPropByte(DEVINST devInst, const DEVPROPKEY* key, int* outVal) {
    DEVPROPTYPE type = 0;
    BYTE val = 0;
    ULONG size = sizeof(val);
    if (CM_Get_DevNode_PropertyW(devInst, key, &type, &val, &size, 0) == CR_SUCCESS &&
        type == DEVPROP_TYPE_BYTE) {
        *outVal = static_cast<int>(val);
        return true;
    }
    return false;
}

static bool ReadDevPropString(DEVINST devInst, const DEVPROPKEY* key, wchar_t* outStr, size_t outChars) {
    DEVPROPTYPE type = 0;
    ULONG size = static_cast<ULONG>(outChars * sizeof(wchar_t));
    if (CM_Get_DevNode_PropertyW(devInst, key, &type, reinterpret_cast<PBYTE>(outStr), &size, 0) == CR_SUCCESS &&
        type == DEVPROP_TYPE_STRING && outStr[0] != L'\0') {
        outStr[outChars - 1] = L'\0';
        return true;
    }
    return false;
}

static bool IsZeroGuid(const GUID& g) {
    static const GUID zero = {};
    return memcmp(&g, &zero, sizeof(GUID)) == 0;
}

static BtContainerEntry* FindOrCreateEntry(BtContainerEntry* entries, int& count, int maxCount,
                                           const GUID* containerId, const wchar_t* macHex) {
    for (int i = 0; i < count; ++i) {
        if (containerId && !IsZeroGuid(*containerId) && entries[i].hasContainer &&
            IsEqualGUID(entries[i].containerId, *containerId)) {
            return &entries[i];
        }
        if (macHex && macHex[0] && entries[i].macHex[0] && _wcsicmp(entries[i].macHex, macHex) == 0) {
            return &entries[i];
        }
    }
    if (count >= maxCount) return nullptr;
    BtContainerEntry& e = entries[count++];
    ZeroMemory(&e, sizeof(e));
    e.used = true;
    e.battery = -1;
    if (containerId && !IsZeroGuid(*containerId)) {
        e.containerId = *containerId;
        e.hasContainer = true;
    }
    if (macHex && macHex[0]) {
        SafeCopyW(e.macHex, ARRAYSIZE(e.macHex), macHex);
    }
    return &e;
}

static void ExtractMacFromInstanceId(const wchar_t* id, wchar_t* outMac, size_t outChars) {
    outMac[0] = L'\0';
    if (!id) return;
    // Patterns: BTHENUM\DEV_XXXXXXXXXXXX\... or ...\7&...&0&XXXXXXXXXXXX_C00000000 or BTHLE\DEV_XXXXXXXXXXXX
    const wchar_t* devPos = wcsstr(id, L"DEV_");
    if (devPos) {
        devPos += 4;
        int len = 0;
        while (len < 12 && iswxdigit(devPos[len])) len++;
        if (len == 12) {
            wcsncpy_s(outMac, outChars, devPos, 12);
            return;
        }
    }
    // Look for 12 hex digits before '_' or end
    for (const wchar_t* p = id; *p; ++p) {
        if (iswxdigit(*p)) {
            int len = 0;
            while (len < 13 && iswxdigit(p[len])) len++;
            if (len == 12 && (p[12] == L'_' || p[12] == L'\0' || p[12] == L'\\')) {
                // Skip all-zeros
                if (_wcsnicmp(p, L"000000000000", 12) != 0) {
                    wcsncpy_s(outMac, outChars, p, 12);
                    return;
                }
            }
            p += (len > 0 ? len - 1 : 0);
        }
    }
}

static bool IsGenericServiceName(const wchar_t* name) {
    if (!name || !name[0]) return true;
    if (ContainsInsensitive(name, L"Служба идентификации")) return true;
    if (ContainsInsensitive(name, L"Device Identification")) return true;
    if (ContainsInsensitive(name, L"Периферийное устройство Bluetooth")) return true;
    if (ContainsInsensitive(name, L"Bluetooth Peripheral")) return true;
    if (ContainsInsensitive(name, L"Bluetooth HID")) return true;
    if (ContainsInsensitive(name, L"Транспорт AVRCP")) return true;
    if (ContainsInsensitive(name, L"Avrcp Transport")) return true;
    if (ContainsInsensitive(name, L"Стандартный последовательный")) return true;
    if (ContainsInsensitive(name, L"Standard Serial")) return true;
    if (ContainsInsensitive(name, L"Hands-Free AG")) return true;
    if (ContainsInsensitive(name, L"UUID128")) return true;
    if (ContainsInsensitive(name, L"Battery Service")) return true;
    if (ContainsInsensitive(name, L"Generic Attribute")) return true;
    if (ContainsInsensitive(name, L"Generic Access")) return true;
    return false;
}

static void StripAudioSuffix(wchar_t* name) {
    size_t len = wcslen(name);
    const wchar_t* suffixes[] = { L" Stereo", L" Hands-Free AG Audio", L" Hands-Free" };
    for (const wchar_t* suf : suffixes) {
        size_t slen = wcslen(suf);
        if (len > slen && _wcsicmp(name + len - slen, suf) == 0) {
            name[len - slen] = L'\0';
            break;
        }
    }
}

static void QueryBleGattBattery(BtContainerEntry* entries, int count) {
    HDEVINFO hDevInfo = SetupDiGetClassDevsW(&GUID_BLE_BATTERY_SERVICE, nullptr, nullptr,
                                            DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (hDevInfo == INVALID_HANDLE_VALUE) return;

    SP_DEVICE_INTERFACE_DATA ifData = {};
    ifData.cbSize = sizeof(ifData);

    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(hDevInfo, nullptr, &GUID_BLE_BATTERY_SERVICE, i, &ifData); ++i) {
        DWORD reqSize = 0;
        SP_DEVINFO_DATA devInfo = {};
        devInfo.cbSize = sizeof(devInfo);
        SetupDiGetDeviceInterfaceDetailW(hDevInfo, &ifData, nullptr, 0, &reqSize, nullptr);
        if (reqSize == 0 || reqSize > 2048) continue;

        BYTE buf[2048] = {};
        auto* detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(buf);
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(hDevInfo, &ifData, detail, reqSize, nullptr, &devInfo)) continue;

        GUID cid = {};
        if (!ReadDevPropGuid(devInfo.DevInst, &DEVPKEY_Device_ContainerId, &cid)) continue;

        // Match only connected entries that don't already have battery
        BtContainerEntry* target = nullptr;
        for (int j = 0; j < count; ++j) {
            if (entries[j].used && entries[j].hasContainer && IsEqualGUID(entries[j].containerId, cid)) {
                target = &entries[j];
                break;
            }
        }
        if (!target || !target->connected || target->battery >= 0) continue;

        HANDLE hGatt = CreateFileW(detail->DevicePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   nullptr, OPEN_EXISTING, 0, nullptr);
        if (hGatt == INVALID_HANDLE_VALUE) continue;

        BTH_LE_GATT_SERVICE svc = {};
        svc.ServiceUuid.IsShortUuid = TRUE;
        svc.ServiceUuid.Value.ShortUuid = 0x180F;

        USHORT charCount = 0;
        HRESULT hr = BluetoothGATTGetCharacteristics(hGatt, &svc, 0, nullptr, &charCount, BLUETOOTH_GATT_FLAG_NONE);
        if (hr == HRESULT_FROM_WIN32(ERROR_MORE_DATA) && charCount > 0 && charCount <= 16) {
            BTH_LE_GATT_CHARACTERISTIC chars[16] = {};
            USHORT actual = 0;
            if (SUCCEEDED(BluetoothGATTGetCharacteristics(hGatt, &svc, charCount, chars, &actual, BLUETOOTH_GATT_FLAG_NONE))) {
                for (USHORT c = 0; c < actual; ++c) {
                    if (chars[c].CharacteristicUuid.IsShortUuid && chars[c].CharacteristicUuid.Value.ShortUuid == 0x2A19) {
                        USHORT valSize = 0;
                        hr = BluetoothGATTGetCharacteristicValue(hGatt, &chars[c], 0, nullptr, &valSize, BLUETOOTH_GATT_FLAG_NONE);
                        if (hr == HRESULT_FROM_WIN32(ERROR_MORE_DATA) && valSize > 0 && valSize <= 256) {
                            BYTE valBuf[256] = {};
                            auto* val = reinterpret_cast<PBTH_LE_GATT_CHARACTERISTIC_VALUE>(valBuf);
                            USHORT actualVal = 0;
                            if (SUCCEEDED(BluetoothGATTGetCharacteristicValue(hGatt, &chars[c], valSize, val, &actualVal, BLUETOOTH_GATT_FLAG_NONE))) {
                                if (val->DataSize >= 1 && val->Data[0] <= 100) {
                                    target->battery = val->Data[0];
                                    target->isLE = true;
                                }
                            }
                        }
                        break;
                    }
                }
            }
        }
        CloseHandle(hGatt);
    }
    SetupDiDestroyDeviceInfoList(hDevInfo);
}

void BtMonitor_CollectDevices(DeviceSnapshot* outSnapshot, bool includeDisconnected) {
    if (!outSnapshot) return;

    static BtContainerEntry entries[MAX_DEVICES];
    ZeroMemory(entries, sizeof(entries));
    int entryCount = 0;

    // Step 1: Enumerate paired classic/LE devices via BluetoothFindFirstDevice (fast, zero radio inquiry)
    BLUETOOTH_DEVICE_SEARCH_PARAMS sp = {};
    sp.dwSize = sizeof(sp);
    sp.fReturnAuthenticated = TRUE;
    sp.fReturnRemembered = TRUE;
    sp.fReturnConnected = TRUE;
    sp.fReturnUnknown = FALSE;
    sp.fIssueInquiry = FALSE;
    sp.cTimeoutMultiplier = 0;

    BLUETOOTH_DEVICE_INFO bdi = {};
    bdi.dwSize = sizeof(bdi);
    HBLUETOOTH_DEVICE_FIND hFind = BluetoothFindFirstDevice(&sp, &bdi);
    if (hFind) {
        do {
            wchar_t macHex[16] = {};
            ULONGLONG addr = bdi.Address.ullLong;
            swprintf_s(macHex, ARRAYSIZE(macHex), L"%02X%02X%02X%02X%02X%02X",
                       (unsigned)((addr >> 40) & 0xFF), (unsigned)((addr >> 32) & 0xFF),
                       (unsigned)((addr >> 24) & 0xFF), (unsigned)((addr >> 16) & 0xFF),
                       (unsigned)((addr >> 8) & 0xFF), (unsigned)(addr & 0xFF));

            BtContainerEntry* e = FindOrCreateEntry(entries, entryCount, MAX_DEVICES, nullptr, macHex);
            if (e) {
                if (bdi.szName[0]) SafeCopyW(e->name, ARRAYSIZE(e->name), bdi.szName);
                e->cod = bdi.ulClassofDevice;
                e->connected = (bdi.fConnected != FALSE);
            }
            ZeroMemory(&bdi, sizeof(bdi));
            bdi.dwSize = sizeof(bdi);
        } while (BluetoothFindNextDevice(hFind, &bdi));
        BluetoothFindDeviceClose(hFind);
    }

    // Step 2: Walk present PnP nodes for BTH* to read ContainerId, IsConnected, Battery, BLE flag
    HDEVINFO hDevInfo = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (hDevInfo != INVALID_HANDLE_VALUE) {
        SP_DEVINFO_DATA devInfo = {};
        devInfo.cbSize = sizeof(devInfo);

        for (DWORD idx = 0; SetupDiEnumDeviceInfo(hDevInfo, idx, &devInfo); ++idx) {
            wchar_t instId[256] = {};
            if (CM_Get_Device_IDW(devInfo.DevInst, instId, ARRAYSIZE(instId), 0) != CR_SUCCESS) continue;

            bool isBthEnum = (_wcsnicmp(instId, L"BTHENUM\\", 8) == 0);
            bool isBthHf   = (_wcsnicmp(instId, L"BTHHFENUM\\", 10) == 0);
            bool isBthLe   = (_wcsnicmp(instId, L"BTHLE\\", 6) == 0 || _wcsnicmp(instId, L"BTHLEDevice\\", 12) == 0);
            if (!isBthEnum && !isBthHf && !isBthLe) continue;

            // Skip adapter/enumerator infrastructure nodes
            if (wcsstr(instId, L"LOCALMFG") != nullptr) continue;

            GUID cid = {};
            bool hasCid = ReadDevPropGuid(devInfo.DevInst, &DEVPKEY_Device_ContainerId, &cid);

            wchar_t macHex[16] = {};
            if (!ReadDevPropString(devInfo.DevInst, &PKEY_BT_ADDR, macHex, ARRAYSIZE(macHex))) {
                ExtractMacFromInstanceId(instId, macHex, ARRAYSIZE(macHex));
            }
            if ((!hasCid || IsZeroGuid(cid)) && macHex[0] == L'\0') continue;

            BtContainerEntry* e = FindOrCreateEntry(entries, entryCount, MAX_DEVICES,
                                                    hasCid ? &cid : nullptr, macHex);
            if (!e) continue;

            if (hasCid && !IsZeroGuid(cid) && !e->hasContainer) {
                e->containerId = cid;
                e->hasContainer = true;
            }
            if (macHex[0] && !e->macHex[0]) {
                SafeCopyW(e->macHex, ARRAYSIZE(e->macHex), macHex);
            }
            if (isBthLe) e->isLE = true;

            bool conn = false;
            if (ReadDevPropBool(devInfo.DevInst, &PKEY_DEV_IS_CONNECTED, &conn) && conn) {
                e->connected = true;
            }

            DWORD cod = 0;
            if (ReadDevPropUint32(devInfo.DevInst, &PKEY_BT_COD, &cod) && cod != 0 && e->cod == 0) {
                e->cod = cod;
            }

            int bat = -1;
            if (ReadDevPropByte(devInfo.DevInst, &PKEY_BT_BATTERY, &bat) && bat >= 0 && bat <= 100) {
                e->battery = bat;
            }

            // Pick best human-readable name
            wchar_t nodeName[128] = {};
            if (!ReadDevPropString(devInfo.DevInst, &DEVPKEY_Device_FriendlyName, nodeName, ARRAYSIZE(nodeName))) {
                ReadDevPropString(devInfo.DevInst, &DEVPKEY_NAME, nodeName, ARRAYSIZE(nodeName));
            }
            if (nodeName[0] && !IsGenericServiceName(nodeName)) {
                StripAudioSuffix(nodeName);
                if (e->name[0] == L'\0' || wcslen(nodeName) < wcslen(e->name)) {
                    SafeCopyW(e->name, ARRAYSIZE(e->name), nodeName);
                }
            }
        }
        SetupDiDestroyDeviceInfoList(hDevInfo);
    }

    // Merge any entries that ended up split across MAC-only (Step 1) and ContainerId-only (BTHHFENUM before BTHENUM)
    for (int i = 0; i < entryCount; ++i) {
        if (!entries[i].used) continue;
        for (int j = i + 1; j < entryCount; ++j) {
            if (!entries[j].used) continue;
            bool sameCid = (entries[i].hasContainer && entries[j].hasContainer &&
                            IsEqualGUID(entries[i].containerId, entries[j].containerId));
            bool sameMac = (entries[i].macHex[0] && entries[j].macHex[0] &&
                            _wcsicmp(entries[i].macHex, entries[j].macHex) == 0);
            if (sameCid || sameMac) {
                if (!entries[i].hasContainer && entries[j].hasContainer) {
                    entries[i].containerId = entries[j].containerId;
                    entries[i].hasContainer = true;
                }
                if (!entries[i].macHex[0] && entries[j].macHex[0]) {
                    SafeCopyW(entries[i].macHex, ARRAYSIZE(entries[i].macHex), entries[j].macHex);
                }
                if (entries[j].name[0] && (!entries[i].name[0] || wcslen(entries[j].name) < wcslen(entries[i].name))) {
                    SafeCopyW(entries[i].name, ARRAYSIZE(entries[i].name), entries[j].name);
                }
                if (entries[i].cod == 0 && entries[j].cod != 0) entries[i].cod = entries[j].cod;
                if (entries[i].battery < 0 && entries[j].battery >= 0) entries[i].battery = entries[j].battery;
                if (entries[j].connected) entries[i].connected = true;
                if (entries[j].isLE) entries[i].isLE = true;
                entries[j].used = false;
            }
        }
    }

    // Step 3: Check BLE GATT Battery Service for connected LE devices missing PnP battery
    QueryBleGattBattery(entries, entryCount);

    // Step 4: Output results
    ULONGLONG now = GetTickCount64();
    for (int i = 0; i < entryCount && outSnapshot->count < MAX_DEVICES; ++i) {
        const BtContainerEntry& e = entries[i];
        if (!e.used || e.name[0] == L'\0') continue;
        if (!e.connected && !includeDisconnected) continue;

        DeviceInfo& dst = outSnapshot->items[outSnapshot->count++];
        ZeroMemory(&dst, sizeof(dst));
        if (e.macHex[0]) {
            swprintf_s(dst.id, ARRAYSIZE(dst.id), L"BT_%ls", e.macHex);
        } else {
            swprintf_s(dst.id, ARRAYSIZE(dst.id), L"BT_CID_%08lX", e.containerId.Data1);
        }
        SafeCopyW(dst.name, ARRAYSIZE(dst.name), e.name);
        dst.type = ClassifyBluetoothDevice(e.cod, e.name);
        dst.conn = e.isLE ? ConnectionKind::BluetoothLE : ConnectionKind::Bluetooth;
        dst.battery = e.battery;
        dst.charging = false;
        dst.connected = e.connected;
        dst.sleeping = false;
        dst.lastUpdateTick = now;
    }
}
