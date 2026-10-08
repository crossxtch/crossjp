#pragma once

#include <cstddef>
#include <cstdint>

// Saved Wi-Fi networks, persisted as a flat binary file (mirrors core/Settings.h).
// Fixed capacity: no dynamic growth, no JSON dependency.
class WifiCredentialStore {
 public:
  static constexpr uint32_t MAGIC = 0x46495758;  // "WXIF" (byte-swapped "WIFX")
  static constexpr uint16_t VERSION = 2;
  static constexpr uint16_t kMinVersion = 1;
  static constexpr const char* kPath = "/.crossjp/wifi.bin";
  static constexpr int kMaxNetworks = 5;
  static constexpr size_t kSsidLen = 33;      // 32 chars + NUL, per 802.11 max SSID length
  static constexpr size_t kPasswordLen = 64;  // 63 chars + NUL, per WPA2 max passphrase length

  struct Entry {
    char ssid[kSsidLen]{};
    char password[kPasswordLen]{};
    bool used = false;
  };

  void load();
  void save() const;

  // Returns nullptr if no saved credential matches ssid.
  const Entry* find(const char* ssid) const;
  bool hasAny() const;
  // Inserts or updates the entry for ssid, evicting the oldest slot if full.
  void addOrUpdate(const char* ssid, const char* password);
  // Returns true if a matching slot was cleared.
  bool remove(const char* ssid);

  void setLastConnected(const char* ssid);
  // Empty string if none.
  const char* lastConnected() const { return lastConnectedSsid; }

  const Entry* entries() const { return slots; }
  int capacity() const { return kMaxNetworks; }

 private:
  uint32_t magic = MAGIC;
  uint16_t version = VERSION;
  uint16_t nextEvict = 0;  // round-robin eviction index when full
  Entry slots[kMaxNetworks]{};
  char lastConnectedSsid[kSsidLen]{};
};

extern WifiCredentialStore wifiCredentials;
