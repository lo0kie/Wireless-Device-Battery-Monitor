#include "known_devices.h"

#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <vector>

namespace known_devices {
namespace {

constexpr size_t kMaxKey = 512;
constexpr size_t kMaxName = 256;
constexpr size_t kMaxVia = 64;

bool ValidField(const std::wstring& s, size_t max) {
    if (s.empty() || s.size() > max) return false;
    for (wchar_t c : s)
        if (c < 32 || c == 127) return false;
    return true;
}

// Flat REG_MULTI_SZ: key\0name\0via\0kind\0 ... \0\0. Four strings per device, like the alias
// list: no delimiters inside a field, so names and keys round-trip unchanged.
std::vector<wchar_t> Encode(const Map& devices) {
    std::vector<wchar_t> data;
    size_t count = 0;
    for (const auto& [key, d] : devices) {
        if (count >= kMaxDevices) break;
        if (!ValidField(key, kMaxKey) || !ValidField(d.name, kMaxName) || d.via.empty() ||
            d.via.size() > kMaxVia)
            continue;
        const std::wstring kind = std::to_wstring((int)d.kind);
        for (const std::wstring* field : { &key, &d.name, &d.via, &kind }) {
            data.insert(data.end(), field->begin(), field->end());
            data.push_back(0);
        }
        ++count;
    }
    data.push_back(0);
    if (data.size() == 1) data.push_back(0);
    return data;
}

Map Decode(const wchar_t* data, size_t count) {
    Map devices;
    if (!data || count < 2 || data[count - 1] != 0 || data[count - 2] != 0) return devices;
    const wchar_t* end = data + count;
    const wchar_t* p = data;
    while (p < end && *p && devices.size() < kMaxDevices) {
        std::wstring field[4];
        for (int i = 0; i < 4; i++) {
            const wchar_t* stop = std::find(p, end, L'\0');
            if (stop == end) return devices;
            field[i].assign(p, stop);
            p = stop + 1;
            if (i < 3 && p == end) return devices;
        }
        wchar_t* kindEnd = nullptr;
        const long k = wcstol(field[3].c_str(), &kindEnd, 10);
        if (kindEnd && !*kindEnd && k >= 0 && k <= (long)DeviceKind::Speaker &&
            ValidField(field[0], kMaxKey) && ValidField(field[1], kMaxName) && !field[2].empty() &&
            field[2].size() <= kMaxVia)
            devices.emplace(field[0], Device{ field[1], field[2], (DeviceKind)k });
    }
    return devices;
}

}  // namespace

Map Load(const wchar_t* settingsKey) {
    Map devices;
    DWORD size = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, settingsKey, L"KnownDevices", RRF_RT_REG_MULTI_SZ, nullptr,
                     nullptr, &size) != ERROR_SUCCESS || size < 2 * sizeof(wchar_t) ||
        size % sizeof(wchar_t))
        return devices;
    std::vector<wchar_t> buf(size / sizeof(wchar_t), 0);
    if (RegGetValueW(HKEY_CURRENT_USER, settingsKey, L"KnownDevices", RRF_RT_REG_MULTI_SZ, nullptr,
                     buf.data(), &size) == ERROR_SUCCESS)
        devices = Decode(buf.data(), buf.size());
    return devices;
}

void Save(const wchar_t* settingsKey, const Map& devices) {
    const std::vector<wchar_t> data = Encode(devices);
    RegSetKeyValueW(HKEY_CURRENT_USER, settingsKey, L"KnownDevices", REG_MULTI_SZ, data.data(),
                    (DWORD)(data.size() * sizeof(wchar_t)));
}

bool Remember(Map& devices, const DeviceStatus& s) {
    if (!ValidField(s.key, kMaxKey) || s.name.empty() || s.name.size() > kMaxName || s.via.empty() ||
        s.via.size() > kMaxVia)
        return false;
    auto it = devices.find(s.key);
    if (it != devices.end()) {
        if (it->second.name == s.name && it->second.via == s.via && it->second.kind == s.kind)
            return false;  // unchanged: no registry write
        it->second = Device{ s.name, s.via, s.kind };
        return true;
    }
    if (devices.size() >= kMaxDevices) return false;
    devices.emplace(s.key, Device{ s.name, s.via, s.kind });
    return true;
}

}  // namespace known_devices
