#pragma once
#include "devices.h"

#include <map>
#include <string>

// The set of devices that have answered at least once. Unlike g_entries, this survives a restart:
// it stores only what is needed to draw a placeholder (key, name, kind, connection) -- never a
// battery percentage, which would be stale and misleading. A device answering in a later poll
// simply overwrites its record.
namespace known_devices {

struct Device {
    std::wstring name;
    std::wstring via;
    DeviceKind kind = DeviceKind::Other;
};

constexpr size_t kMaxDevices = 128;

using Map = std::map<std::wstring, Device>;

// Reads the archive from `settingsKey` (the app's HKCU settings path). Malformed or truncated
// records are discarded; a missing value means a fresh install.
Map Load(const wchar_t* settingsKey);

// Writes the whole archive back. Called only when a record actually changed, not every poll.
void Save(const wchar_t* settingsKey, const Map& devices);

// Adds or updates `s`. Returns true only when the stored identity changed, so callers can avoid
// touching the registry on every poll. Full archives drop new devices, like the alias list does.
bool Remember(Map& devices, const DeviceStatus& s);

}  // namespace known_devices
