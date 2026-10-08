#pragma once

#include <cstdint>

struct Settings {
  static constexpr uint32_t MAGIC = 0x48585443;  // "HXTC"
  static constexpr uint16_t VERSION = 9;
  static constexpr uint16_t kMinVersion = 3;
  static constexpr const char* kDir = "/.crossjp";
  static constexpr const char* kPath = "/.crossjp/settings.bin";
  // trueSleepMinutes: 0 = none, else 5 / 10 / 15 minutes.
  static constexpr uint8_t kSleepNone = 0;
  static constexpr uint8_t kSleep5Min = 5;
  static constexpr uint8_t kSleep10Min = 10;
  static constexpr uint8_t kSleep15Min = 15;
  static constexpr uint8_t kLanguageEnglish = 0;
  static constexpr uint8_t kLanguageJapanese = 1;
  static constexpr uint8_t kLanguageChinese = 2;
  static constexpr uint8_t kLanguageUnset = 0xFF;

  uint32_t magic = MAGIC;
  uint16_t version = VERSION;
  uint8_t gyroAutoOffSeconds = 60;  // IMU auto-lock; 0 = none, else 30/45/60. Same slot as old clockModeSeconds.
  uint8_t refreshEveryNPages = 5;
  uint8_t nightMode = 0;
  uint8_t tiltPageTurn = 0;  // 0 = off, 1 = on (flick either direction pages forward, X3 only)
  char lastBookPath[200]{};
  uint8_t trueSleepMinutes = kSleepNone;  // deep sleep; see kSleep*
  uint8_t clockHasBeenSynced = 0;  // set after a successful NTP write to the RTC
  uint8_t clockUtcOffsetQ = 48;    // 48 = UTC+0; 15-minute steps, 0 = UTC-12, 104 = UTC+14
  uint16_t ntpSyncYear = 0;        // UTC year of last NTP; 0 = never
  uint8_t ntpSyncMonth = 0;        // 1-12; NTP at most once per calendar month
  char fontFile[80]{};             // basename in /.crossjp/fonts/; empty = first installed
  uint8_t language = kLanguageUnset;  // kLanguageEnglish/Japanese/Chinese; unset until first pick

  void load();
  void save() const;
  unsigned long gyroAutoOffTimeoutMs() const;
  unsigned long trueSleepTimeoutMs() const;
  bool languageChosen() const { return language <= kLanguageChinese; }
};

extern Settings settings;
