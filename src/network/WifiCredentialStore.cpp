#include "network/WifiCredentialStore.h"

#include <HalStorage.h>
#include <Logging.h>

#include <cstdio>
#include <cstring>

WifiCredentialStore wifiCredentials;

namespace {
bool sameSsid(const char* a, const char* b) { return a && b && strncmp(a, b, WifiCredentialStore::kSsidLen) == 0; }

// On-disk v1 layout (no lastConnectedSsid). sizeof includes alignment padding.
struct WifiCredentialV1 {
  uint32_t magic;
  uint16_t version;
  uint16_t nextEvict;
  WifiCredentialStore::Entry slots[WifiCredentialStore::kMaxNetworks];
};
}  // namespace

void WifiCredentialStore::load() {
  WifiCredentialStore loaded{};
  HalFile f;
  if (!Storage.openFileForRead("WIFI", kPath, f)) {
    LOG_INF("WIFI", "No saved networks");
    return;
  }
  const size_t n = static_cast<size_t>(f.read(reinterpret_cast<uint8_t*>(&loaded), sizeof(loaded)));
  if (loaded.magic != MAGIC || loaded.version < kMinVersion || loaded.version > VERSION) {
    LOG_ERR("WIFI", "Ignoring wifi file (n=%u magic=0x%08lX ver=%u)", static_cast<unsigned>(n),
            static_cast<unsigned long>(loaded.magic), loaded.version);
    return;
  }
  if (loaded.version == 1) {
    if (n != sizeof(WifiCredentialV1)) {
      LOG_ERR("WIFI", "Ignoring v1 wifi file (n=%u want %u)", static_cast<unsigned>(n),
              static_cast<unsigned>(sizeof(WifiCredentialV1)));
      return;
    }
    loaded.version = VERSION;
  } else if (n != sizeof(loaded)) {
    LOG_ERR("WIFI", "Ignoring wifi file (n=%u want %u)", static_cast<unsigned>(n),
            static_cast<unsigned>(sizeof(loaded)));
    return;
  }
  *this = loaded;
  for (auto& slot : slots) {
    slot.ssid[sizeof(slot.ssid) - 1] = '\0';
    slot.password[sizeof(slot.password) - 1] = '\0';
  }
  lastConnectedSsid[sizeof(lastConnectedSsid) - 1] = '\0';
  LOG_INF("WIFI", "Loaded saved networks");
}

void WifiCredentialStore::save() const {
  Storage.ensureDirectoryExists("/.crossjp");
  HalFile f;
  if (!Storage.openFileForWrite("WIFI", kPath, f)) {
    LOG_ERR("WIFI", "Could not write %s", kPath);
    return;
  }
  const size_t n = f.write(this, sizeof(*this));
  if (n != sizeof(*this)) {
    LOG_ERR("WIFI", "Short wifi write (%u of %u)", static_cast<unsigned>(n), static_cast<unsigned>(sizeof(*this)));
  }
}

const WifiCredentialStore::Entry* WifiCredentialStore::find(const char* ssid) const {
  if (!ssid) {
    return nullptr;
  }
  for (const auto& slot : slots) {
    if (slot.used && sameSsid(slot.ssid, ssid)) {
      return &slot;
    }
  }
  return nullptr;
}

bool WifiCredentialStore::hasAny() const {
  for (const auto& slot : slots) {
    if (slot.used) {
      return true;
    }
  }
  return false;
}

void WifiCredentialStore::addOrUpdate(const char* ssid, const char* password) {
  if (!ssid || ssid[0] == '\0') {
    return;
  }

  for (auto& slot : slots) {
    if (slot.used && sameSsid(slot.ssid, ssid)) {
      snprintf(slot.password, sizeof(slot.password), "%s", password ? password : "");
      save();
      return;
    }
  }

  for (auto& slot : slots) {
    if (!slot.used) {
      snprintf(slot.ssid, sizeof(slot.ssid), "%s", ssid);
      snprintf(slot.password, sizeof(slot.password), "%s", password ? password : "");
      slot.used = true;
      save();
      return;
    }
  }

  // Full: evict round-robin so one bad network can't permanently block new saves.
  Entry& victim = slots[nextEvict % kMaxNetworks];
  if (sameSsid(victim.ssid, lastConnectedSsid)) {
    lastConnectedSsid[0] = '\0';
  }
  snprintf(victim.ssid, sizeof(victim.ssid), "%s", ssid);
  snprintf(victim.password, sizeof(victim.password), "%s", password ? password : "");
  victim.used = true;
  nextEvict = (nextEvict + 1) % kMaxNetworks;
  save();
}

bool WifiCredentialStore::remove(const char* ssid) {
  if (!ssid) {
    return false;
  }
  for (auto& slot : slots) {
    if (slot.used && sameSsid(slot.ssid, ssid)) {
      if (sameSsid(lastConnectedSsid, ssid)) {
        lastConnectedSsid[0] = '\0';
      }
      slot = Entry{};
      save();
      return true;
    }
  }
  return false;
}

void WifiCredentialStore::setLastConnected(const char* ssid) {
  if (!ssid) {
    lastConnectedSsid[0] = '\0';
    save();
    return;
  }
  snprintf(lastConnectedSsid, sizeof(lastConnectedSsid), "%s", ssid);
  save();
}
