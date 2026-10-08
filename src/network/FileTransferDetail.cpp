#include "network/FileTransferDetail.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <cstring>
#include <vector>

#include "core/BookCache.h"
#include "core/ReadingFont.h"

namespace xfer {

// Appends `s` to `out`, escaping '"' and '\' so it stays valid inside a JSON string.
void appendJsonEscaped(std::string& out, const char* s) {
  for (const char* p = s; *p; ++p) {
    if (*p == '"' || *p == '\\') {
      out.push_back('\\');
    }
    out.push_back(*p);
  }
}

std::string joinPath(const char* dir, const char* name) {
  if (!dir || dir[0] == '\0' || (dir[0] == '/' && dir[1] == '\0')) {
    std::string out = "/";
    out += name ? name : "";
    return out;
  }
  std::string out = dir;
  if (out.back() != '/') {
    out += '/';
  }
  out += name ? name : "";
  return out;
}

// Directory portion of `path` (parent folder). "/" if path has no parent.
std::string dirnameOf(const char* path) {
  const char* slash = strrchr(path, '/');
  if (!slash || slash == path) {
    return "/";
  }
  return std::string(path, static_cast<size_t>(slash - path));
}

// Final path segment (file/folder name) of `path`.
const char* basenameOf(const char* path) {
  const char* slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}

bool isProtectedPath(const char* path) {
  const char* name = basenameOf(path);
  return name[0] == '.' || strcmp(name, "System Volume Information") == 0;
}

bool isPartialName(const char* name) {
  if (!name) {
    return false;
  }
  const size_t n = strlen(name);
  constexpr char kSuffix[] = ".partial";
  constexpr size_t kLen = sizeof(kSuffix) - 1;
  return n >= kLen && memcmp(name + (n - kLen), kSuffix, kLen) == 0;
}

// Decimal only. Empty and any non-digit fail so a bad header cannot resume
// at the wrong place.
bool parseU64(const char* s, uint64_t& out) {
  if (!s || *s < '0' || *s > '9') {
    return false;
  }
  uint64_t n = 0;
  for (const char* p = s; *p; ++p) {
    if (*p < '0' || *p > '9') {
      return false;
    }
    const uint64_t digit = static_cast<uint64_t>(*p - '0');
    if (n > (UINT64_MAX - digit) / 10) {
      return false;
    }
    n = n * 10 + digit;
  }
  out = n;
  return true;
}

// The macOS save panel appends ".xgf2" again when the suggested name already
// ends in it, so "Face-x3.xgf2" arrives as "Face-x3.xgf2.xgf2".
bool tailHasEocd(const uint8_t* tail, const uint32_t n) {
  if (n < 22) {
    return false;
  }
  for (int i = static_cast<int>(n) - 22; i >= 0; --i) {
    if (tail[i] == 0x50 && tail[i + 1] == 0x4b && tail[i + 2] == 0x05 && tail[i + 3] == 0x06) {
      return true;
    }
  }
  return false;
}

bool endsWithIgnoreCase(const char* s, size_t n, const char* ext) {
  const size_t e = strlen(ext);
  if (n < e) {
    return false;
  }
  for (size_t i = 0; i < e; ++i) {
    char c = s[n - e + i];
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
    if (c != ext[i]) {
      return false;
    }
  }
  return true;
}

// Final path for an upload. Fonts always land in /.crossjp/fonts.
bool resolveUploadDest(const char* dir, const char* name, std::string& dest) {
  if (!name || name[0] == '\0' || strchr(name, '/') || strchr(name, '\\')) {
    return false;
  }
  const char* ext = strrchr(name, '.');
  if (ext && strcasecmp(ext, ".xgf2") == 0) {
    const char* fontName = name;
    char trimmed[ReadingFont::kMaxFileName + 8];
    const size_t n = strlen(name);
    if (endsWithIgnoreCase(name, n, ".xgf2.xgf2") && n - 5 < sizeof(trimmed)) {
      memcpy(trimmed, name, n - 5);
      trimmed[n - 5] = '\0';
      fontName = trimmed;
    }
    ReadingFont::migrate();
    char fileName[ReadingFont::kMaxFileName + 1];
    if (!ReadingFont::copyFilename(fileName, sizeof(fileName), fontName)) {
      return false;
    }
    char path[192];
    ReadingFont::makePath(path, sizeof(path), fileName);
    dest = path;
    return true;
  }
  dest = joinPath(!dir || dir[0] == '\0' ? "/" : dir, name);
  return !dest.empty();
}

void pumpNetwork() {
  feedLoopWDT();
  yield();
}

// Removes directory contents (including hidden files the file list hides), then
// the caller rmdirs `path`. Collects names first so we never mutate a directory
// while iterating it. Recurses only for nested folders.
bool deleteDirContents(const char* path) {
  std::vector<std::string> files;
  std::vector<std::string> dirs;
  files.reserve(32);
  dirs.reserve(8);
  {
    HalFile dir = Storage.open(path);
    if (!dir || !dir.isDirectory()) {
      return false;
    }
    auto name = makeUniqueNoThrow<char[]>(HalFile::kMaxNameBytes);
    if (!name) {
      LOG_ERR("XFER", "OOM: delete name");
      return false;
    }
    for (HalFile file = dir.openNextFile(); file; file = dir.openNextFile()) {
      if (file.getName(name.get(), HalFile::kMaxNameBytes) == 0) {
        LOG_ERR("XFER", "Unreadable name in %s", path);
        return false;
      }
      if (file.isDirectory()) {
        dirs.emplace_back(name.get());
      } else {
        files.emplace_back(name.get());
      }
    }
  }
  for (const auto& name : files) {
    pumpNetwork();
    const std::string full = joinPath(path, name.c_str());
    if (!Storage.remove(full.c_str())) {
      LOG_ERR("XFER", "Failed to remove %s", full.c_str());
      return false;
    }
    BookCache::removeFor(full.c_str());
  }
  for (const auto& name : dirs) {
    pumpNetwork();
    const std::string full = joinPath(path, name.c_str());
    if (!deleteDirContents(full.c_str()) || !Storage.rmdir(full.c_str())) {
      LOG_ERR("XFER", "Failed to remove dir %s", full.c_str());
      return false;
    }
  }
  return true;
}

}  // namespace xfer
