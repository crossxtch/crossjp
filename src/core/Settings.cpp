#include "core/Settings.h"

#include <HalStorage.h>
#include <Logging.h>

#include <cstring>

Settings settings;

void Settings::load() {
  Settings loaded{};
  HalFile f;
  if (!Storage.openFileForRead("SET", kPath, f)) {
    LOG_INF("SET", "No settings file, using defaults");
    return;
  }
  const size_t n = static_cast<size_t>(f.read(reinterpret_cast<uint8_t*>(&loaded), sizeof(loaded)));
  if (n < 10 || loaded.magic != MAGIC || loaded.version < kMinVersion || loaded.version > VERSION) {
    LOG_ERR("SET", "Ignoring settings file (n=%u magic=0x%08lX ver=%u, need %u/0x%08lX/%u-%u)",
            static_cast<unsigned>(n), static_cast<unsigned long>(loaded.magic), loaded.version,
            static_cast<unsigned>(sizeof(loaded)), static_cast<unsigned long>(MAGIC), kMinVersion, VERSION);
    return;
  }
  if (loaded.version < 4) {
    loaded.trueSleepMinutes = 0;
  }
  if (loaded.version < 5) {
    loaded.clockHasBeenSynced = 0;
  }
  if (loaded.version < 6) {
    loaded.clockUtcOffsetQ = 48;
  }
  if (loaded.version < 7) {
    loaded.ntpSyncYear = 0;
    loaded.ntpSyncMonth = 0;
  }
  if (loaded.version < 8) {
    loaded.fontFile[0] = '\0';
  }
  if (loaded.version < 9) {
    loaded.reservedLanguage = 0;
  }
  loaded.version = VERSION;
  *this = loaded;
  lastBookPath[sizeof(lastBookPath) - 1] = '\0';
  fontFile[sizeof(fontFile) - 1] = '\0';
  if (gyroAutoOffSeconds != 0 && gyroAutoOffSeconds != 30 && gyroAutoOffSeconds != 45 &&
      gyroAutoOffSeconds != 60) {
    gyroAutoOffSeconds = 60;
  }
  if (trueSleepMinutes != kSleepNone && trueSleepMinutes != kSleep5Min && trueSleepMinutes != kSleep10Min &&
      trueSleepMinutes != kSleep15Min) {
    trueSleepMinutes = kSleep15Min;
  }
  if (clockUtcOffsetQ > 104) {
    clockUtcOffsetQ = 48;
  }
  LOG_INF("SET",
          "Loaded gyroOff=%u sec sleep=%u refresh=%u night=%u tilt=%u clock=%u tzq=%u font='%s' last='%s'",
          gyroAutoOffSeconds, trueSleepMinutes, refreshEveryNPages, nightMode, tiltPageTurn, clockHasBeenSynced,
          clockUtcOffsetQ, fontFile, lastBookPath);
}

void Settings::save() const {
  Storage.ensureDirectoryExists(kDir);
  HalFile f;
  if (!Storage.openFileForWrite("SET", kPath, f)) {
    LOG_ERR("SET", "Could not write %s", kPath);
    return;
  }
  const size_t n = f.write(this, sizeof(*this));
  if (n != sizeof(*this)) {
    LOG_ERR("SET", "Short settings write (%u of %u)", static_cast<unsigned>(n), static_cast<unsigned>(sizeof(*this)));
    return;
  }
  LOG_DBG("SET", "Saved gyroOff=%u sleep=%u refresh=%u night=%u tilt=%u last='%s'", gyroAutoOffSeconds,
          trueSleepMinutes, refreshEveryNPages, nightMode, tiltPageTurn, lastBookPath);
}

namespace {
unsigned long minutesToMs(const uint8_t minutes) {
  if (minutes == 0) {
    return 0;
  }
  return static_cast<unsigned long>(minutes) * 60UL * 1000UL;
}
}  // namespace

unsigned long Settings::gyroAutoOffTimeoutMs() const {
  if (gyroAutoOffSeconds == 0) {
    return 0;
  }
  return static_cast<unsigned long>(gyroAutoOffSeconds) * 1000UL;
}

unsigned long Settings::trueSleepTimeoutMs() const { return minutesToMs(trueSleepMinutes); }
