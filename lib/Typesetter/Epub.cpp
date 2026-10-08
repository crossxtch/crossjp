#include "Epub.h"

#include "HtmlSax.h"

#include <Logging.h>
#include <Utf8.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ts {
static_assert(sizeof(EpubBook::Spine) <= 80, "spine slot must leave room for the deflate window");
namespace {

void dirnameOf(char* out, const size_t cap, const char* path) {
  snprintf(out, cap, "%s", path);
  char* slash = strrchr(out, '/');
  if (slash) {
    slash[1] = 0;
  } else {
    out[0] = 0;
  }
}

int hexVal(const char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

void percentDecodeInPlace(char* s) {
  char* w = s;
  for (char* r = s; *r; ++r) {
    if (r[0] == '%' && hexVal(r[1]) >= 0 && hexVal(r[2]) >= 0) {
      *w++ = static_cast<char>((hexVal(r[1]) << 4) | hexVal(r[2]));
      r += 2;
    } else if (*r == '\\') {
      *w++ = '/';
    } else {
      *w++ = *r;
    }
  }
  *w = 0;
}

void joinPath(char* out, const size_t cap, const char* dir, const char* rel) {
  char decoded[96];
  snprintf(decoded, sizeof(decoded), "%s", rel);
  percentDecodeInPlace(decoded);
  const char* r = decoded;
  while (r[0] == '.' && r[1] == '/') {
    r += 2;
  }
  if (r[0] == '/') {
    snprintf(out, cap, "%s", r + 1);
    return;
  }
  snprintf(out, cap, "%s%s", dir ? dir : "", r);
}

bool isHtmlType(const char* t) {
  if (!t || !t[0]) {
    return true;
  }
  return strstr(t, "html") != nullptr || (strstr(t, "xml") != nullptr && strstr(t, "ncx") == nullptr);
}

bool isImageType(const char* t) { return t && strncmp(t, "image/", 6) == 0; }

bool isSvgType(const char* t) { return t && strstr(t, "svg") != nullptr; }

void joinImagePath(char* out, const size_t cap, const char* dir, const char* rel) {
  char joined[160];
  joinPath(joined, sizeof(joined), dir, rel);
  char* seg[32];
  int n = 0;
  char* p = joined;
  while (*p == '/') {
    ++p;
  }
  while (*p && n < 32) {
    char* slash = strchr(p, '/');
    if (slash) {
      *slash = 0;
    }
    if (strcmp(p, ".") == 0) {
    } else if (strcmp(p, "..") == 0) {
      if (n > 0) {
        --n;
      }
    } else if (p[0]) {
      seg[n++] = p;
    }
    if (!slash) {
      break;
    }
    p = slash + 1;
  }
  size_t used = 0;
  if (cap == 0) {
    return;
  }
  out[0] = 0;
  for (int i = 0; i < n; ++i) {
    const size_t len = strlen(seg[i]);
    if (used + len + 2 >= cap) {
      break;
    }
    if (used) {
      out[used++] = '/';
    }
    memcpy(out + used, seg[i], len);
    used += len;
    out[used] = 0;
  }
}

void stripHash(char* s);

const ZipArchive::Entry* findSpineFile(ZipArchive& zip, const char* opfDir, const char* href) {
  char full[128];
  joinPath(full, sizeof(full), opfDir, href ? href : "");
  const ZipArchive::Entry* ent = zip.find(full);
  if (!ent && href && href[0]) {
    ent = zip.find(href);
  }
  if (!ent && href && href[0]) {
    const char* base = strrchr(href, '/');
    base = base ? base + 1 : href;
    ent = zip.findSuffix(base);
  }
  return ent;
}

const ZipArchive::Entry* findImage(ZipArchive& zip, const char* chapterDir, const char* src) {
  char rel[128];
  snprintf(rel, sizeof(rel), "%s", src ? src : "");
  stripHash(rel);
  if (char* q = strchr(rel, '?')) {
    *q = 0;
  }
  if (rel[0] == 0) {
    return nullptr;
  }
  char full[160];
  joinImagePath(full, sizeof(full), chapterDir, rel);
  const ZipArchive::Entry* ent = zip.find(full);
  if (!ent) {
    ent = zip.find(rel);
  }
  if (!ent) {
    const char* base = strrchr(full[0] ? full : rel, '/');
    base = base ? base + 1 : (full[0] ? full : rel);
    if (base[0]) {
      ent = zip.findSuffix(base);
    }
  }
  return ent;
}

uint16_t addZipImage(ZipArchive& zip, PictureWriter& pics, const ZipArchive::Entry& ent) {
  if (strstr(ent.name, ".svg") || strstr(ent.name, ".SVG")) {
    return 0;
  }
  if (!zip.extractToFile(ent, "/.crossjp/work.img")) {
    LOG_ERR("EPUB", "image extract %s", ent.name);
    return 0;
  }
  HalFile hf;
  if (!Storage.openFileForRead("EPUB", "/.crossjp/work.img", hf)) {
    return 0;
  }
  hf.probeContiguous();
  uint8_t mag[8]{};
  const int got = hf.read(mag, sizeof(mag));
  if (got < 4 || !hf.seekSet(0)) {
    return 0;
  }
  const bool jpeg = mag[0] == 0xFF && mag[1] == 0xD8;
  const bool png = mag[0] == 0x89 && mag[1] == 'P' && mag[2] == 'N' && mag[3] == 'G';
  const uint16_t id = jpeg ? pics.addJpeg(hf) : (png ? pics.addPng(hf) : 0);
  if (id == 0) {
    LOG_INF("EPUB", "image skipped %s", ent.name);
  }
  return id;
}

bool isNcxType(const char* t) { return t && strstr(t, "ncx") != nullptr; }

void stripHash(char* s) {
  char* h = strchr(s, '#');
  if (h) {
    *h = 0;
  }
}

bool hrefMatch(const char* spine, const char* tocSrc) {
  char a[96];
  char b[96];
  snprintf(a, sizeof(a), "%s", spine ? spine : "");
  snprintf(b, sizeof(b), "%s", tocSrc ? tocSrc : "");
  percentDecodeInPlace(a);
  percentDecodeInPlace(b);
  stripHash(b);
  if (a[0] == 0 || b[0] == 0) {
    return false;
  }
  if (strcmp(a, b) == 0) {
    return true;
  }
  const char* ba = strrchr(a, '/');
  const char* bb = strrchr(b, '/');
  ba = ba ? ba + 1 : a;
  bb = bb ? bb + 1 : b;
  return strcmp(ba, bb) == 0;
}

void localName(char* out, const size_t cap, const char* path) {
  const char* slash = strrchr(path, '/');
  snprintf(out, cap, "%s", slash ? slash + 1 : path);
}

// A package or TOC bigger than this is inflated onto the card. Holding it
// beside the zip central directory leaves no contiguous block for the spine.
constexpr uint32_t kMemExtractMax = 24576;

struct XmlCursor {
  const char* mem = nullptr;
  size_t memLen = 0;
  HalFile* file = nullptr;
};

int pullHal(void* ctx, char* dst, const int max) {
  if (!ctx || max <= 0) {
    return 0;
  }
  return static_cast<HalFile*>(ctx)->read(dst, static_cast<size_t>(max));
}

bool rewindXml(void* ctx, HtmlSax* sax) {
  auto* cur = static_cast<XmlCursor*>(ctx);
  if (!sax || !cur) {
    return false;
  }
  if (cur->file) {
    if (!cur->file->seekSet(0)) {
      return false;
    }
    sax->bindPull(pullHal, cur->file);
    return true;
  }
  sax->bind(cur->mem ? cur->mem : "", cur->memLen);
  return true;
}

// Memory when the member is small. Otherwise stream to `workPath` and leave
// `file` open at the start. `mem` is malloc'd; the caller frees it.
bool openMember(ZipArchive& zip, const ZipArchive::Entry& ent, const char* workPath, uint8_t*& mem, size_t& memLen,
                HalFile& file, bool& onDisk) {
  mem = nullptr;
  memLen = 0;
  onDisk = false;
  if (ent.uncompSize > 0 && ent.uncompSize <= kMemExtractMax) {
    mem = zip.extract(ent, &memLen);
    if (mem) {
      return true;
    }
  }
  LOG_INF("EPUB", "stream %s uncomp=%lu comp=%lu", ent.name ? ent.name : "?",
          static_cast<unsigned long>(ent.uncompSize), static_cast<unsigned long>(ent.compSize));
  if (!zip.extractToFile(ent, workPath)) {
    return false;
  }
  if (!Storage.openFileForRead("EPUB", workPath, file) || file.fileSize() == 0) {
    LOG_ERR("EPUB", "work open %s", ent.name ? ent.name : "?");
    return false;
  }
  file.probeContiguous();
  onDisk = true;
  return true;
}

// True when the sample names a writing mode. Leaves `chapterMode` alone otherwise.
bool takeWritingMode(const char* xml, const size_t n, const WritingMode bookMode, WritingMode& chapterMode) {
  WritingMode found = bookMode;
  if (!xml || n == 0 || !sniffWritingMode(xml, n, found)) {
    return false;
  }
  chapterMode = found;
  return true;
}

void writeChapterMode(AtomWriter& writer, const WritingMode chapterMode) {
  Atom modeAtom{};
  modeAtom.kind = AtomKind::Mode;
  modeAtom.cp = chapterMode == WritingMode::HorizontalTb ? 1u : 0u;
  writer.write(modeAtom);
}

}  // namespace

bool EpubBook::open(const char* epubPath, const char* atomPath, ProgressFn progress, void* progressCtx,
                    const uint16_t pageW, const uint16_t pageH, const char* picturePath, const uint16_t spineBegin,
                    const uint16_t maxSpines, const bool appendAtoms, const uint32_t atomKeep) {
  close();
  const bool wholeBook = spineBegin == 0 && maxSpines == 0xFFFF && !appendAtoms;
  const unsigned long t0 = millis();
  auto report = [&](uint16_t done, uint16_t total) {
    if (progress) {
      progress(progressCtx, done, total);
    }
    yield();
  };
  report(0, 0);
  if (!zip.open(epubPath)) {
    error = zip.lastError();
    return false;
  }
  LOG_INF("EPUB", "zip %u files %lums", zip.count(), millis() - t0);
  char opfName[96]{};
  if (!parseContainer(opfName, sizeof(opfName))) {
    close();
    return false;
  }
  const ZipArchive::Entry* opfEnt = zip.find(opfName);
  if (!opfEnt) {
    error = "opf missing";
    close();
    return false;
  }
  uint8_t* opf = nullptr;
  size_t opfLen = 0;
  HalFile opfFile;
  bool opfOnDisk = false;
  if (!openMember(zip, *opfEnt, "/.crossjp/work.opf", opf, opfLen, opfFile, opfOnDisk)) {
    error = zip.lastError()[0] ? zip.lastError() : "opf read";
    close();
    return false;
  }
  char opfDir[96]{};
  dirnameOf(opfDir, sizeof(opfDir), opfName);
  XmlCursor opfCur;
  opfCur.mem = reinterpret_cast<const char*>(opf);
  opfCur.memLen = opfLen;
  opfCur.file = opfOnDisk ? &opfFile : nullptr;
  const bool okOpf = parseOpf(rewindXml, &opfCur, opfDir);
  free(opf);
  opfFile.close();
  if (!okOpf) {
    close();
    return false;
  }
  LOG_INF("EPUB", "opf %s spine=%u", opfName, nSpine);

  // Font handle is already released. Drop the zip too so this create is the
  // only open SD file.
  zip.releaseFile();
  AtomWriter writer;
  const bool writerOk = appendAtoms ? writer.openAppend(atomPath, atomKeep) : writer.open(atomPath);
  if (!writerOk) {
    error = "atom file";
    close();
    return false;
  }
  if (!zip.reopenFile()) {
    error = zip.lastError();
    writer.close();
    close();
    return false;
  }

  // A plain character atom is 7 bytes and this xhtml runs about 4 bytes per
  // character, so the sidecar outgrows the chapters. Compare before the
  // 140-second walk. open() already truncated a partial sidecar from the last try.
  uint64_t htmlBytes = 0;
  for (uint16_t i = 0; i < nSpine; ++i) {
    if (items[i].kind != Spine::kHtml || items[i].zipIndex == 0xFFFFu || items[i].zipIndex >= zip.count()) {
      continue;
    }
    htmlBytes += zip.at(items[i].zipIndex).uncompSize;
  }
  uint64_t freeB = 0;
  if (!appendAtoms && Storage.freeBytes(&freeB)) {
    LOG_INF("EPUB", "room free=%lu KB html=%lu KB", static_cast<unsigned long>(freeB / 1024),
            static_cast<unsigned long>(htmlBytes / 1024));
    if (htmlBytes > 0 && freeB < htmlBytes) {
      error = "disk full";
      LOG_ERR("EPUB", "disk full free=%lu KB html=%lu KB", static_cast<unsigned long>(freeB / 1024),
              static_cast<unsigned long>(htmlBytes / 1024));
      writer.close();
      if (atomPath && atomPath[0]) {
        Storage.remove(atomPath);
      }
      close();
      return false;
    }
  }

  PictureWriter pics;
  const bool picsOn = pageW > 0 && pageH > 0 && picturePath && picturePath[0] &&
                      pics.begin(picturePath, pageW, pageH, appendAtoms);
  struct Ctx {
    AtomWriter* w;
    ZipArchive* zip;
    PictureWriter* pics;
    char chapterDir[128];
    bool ok;
    uint16_t ch;
  } ctx{&writer, &zip, picsOn ? &pics : nullptr, {}, true, 0};
  AtomSink sink{};
  sink.ctx = &ctx;
  sink.emit = [](void* c, const Atom& a, uint32_t) -> bool {
    auto* x = static_cast<Ctx*>(c);
    if (!x->w->write(a)) {
      x->ok = false;
      return false;
    }
    if (a.kind == AtomKind::Ch || a.kind == AtomKind::Tcy || a.kind == AtomKind::Group) {
      if (x->ch < 0xFFFFu) {
        ++x->ch;
      }
    }
    return true;
  };
  sink.picture = [](void* c, const char* src) -> uint16_t {
    auto* x = static_cast<Ctx*>(c);
    if (!x->pics || !src || !src[0]) {
      return 0;
    }
    const ZipArchive::Entry* ent = findImage(*x->zip, x->chapterDir, src);
    if (!ent) {
      return 0;
    }
    return addZipImage(*x->zip, *x->pics, *ent);
  };

  // A partial slice learns the TOC before inflating, then frees that document
  // inside loadTocDoc. The whole-book path still loads it after the chapters so
  // the NCX does not sit beside the deflate window on a long series.
  bool tocLoaded = false;
  if (!wholeBook) {
    tocLoaded = loadTocDoc(opfDir);
  }

  uint16_t htmlOk = 0;
  uint32_t atomStart = writer.position();
  unsigned long inflateMs = 0;
  unsigned long saxMs = 0;
  const uint16_t begin = spineBegin > nSpine ? nSpine : spineBegin;
  const uint16_t end = (maxSpines == 0xFFFF || static_cast<uint32_t>(begin) + maxSpines > nSpine)
                           ? nSpine
                           : static_cast<uint16_t>(begin + maxSpines);
  sliceEnd = begin;
  for (uint16_t i = begin; i < end; ++i) {
    if (!writer.ok()) {
      break;
    }
    report(i, nSpine);
    items[i].atomOff = writer.position();
    ctx.ch = 0;
    ctx.chapterDir[0] = 0;
    const bool indexed = items[i].zipIndex != 0xFFFFu && items[i].zipIndex < zip.count();
    const ZipArchive::Entry* ent = indexed ? &zip.at(items[i].zipIndex) : nullptr;
    if (items[i].kind == Spine::kImage || items[i].kind == Spine::kSvg) {
      uint16_t id = 0;
      if (picsOn && items[i].kind != Spine::kSvg && ent) {
        id = addZipImage(zip, pics, *ent);
      }
      Atom a{};
      a.kind = id != 0 ? AtomKind::Picture : AtomKind::PageBreak;
      a.cp = id;
      writer.write(a);
      items[i].done = 1;
      sliceEnd = static_cast<uint16_t>(i + 1);
      continue;
    }
    if (items[i].kind != Spine::kHtml) {
      LOG_INF("EPUB", "skip %s", ent ? ent->name : "?");
      items[i].done = 1;
      sliceEnd = static_cast<uint16_t>(i + 1);
      continue;
    }
    if (!ent) {
      LOG_ERR("EPUB", "spine missing %u", i);
      items[i].done = 1;
      sliceEnd = static_cast<uint16_t>(i + 1);
      continue;
    }
    dirnameOf(ctx.chapterDir, sizeof(ctx.chapterDir), ent->name);
    HtmlIRResult ir{};
    const unsigned long tInf = millis();
    bool parsed = false;
    WritingMode chapterMode = mode;
    bool sniffed = false;
    size_t n = 0;
    uint8_t* xml = nullptr;
    HalFile hf;
    bool onDisk = false;
    if (!openMember(zip, *ent, "/.crossjp/work.xhtml", xml, n, hf, onDisk)) {
      LOG_ERR("EPUB", "extract %s (%u bytes): %s", ent->name, static_cast<unsigned>(ent->uncompSize), zip.lastError());
      items[i].done = 1;
      sliceEnd = static_cast<uint16_t>(i + 1);
      continue;
    }
    if (!onDisk && xml && n > 0) {
      inflateMs += millis() - tInf;
      sniffed = takeWritingMode(reinterpret_cast<char*>(xml), n, mode, chapterMode);
      items[i].atomOff = writer.position();
      writeChapterMode(writer, chapterMode);
      const unsigned long tSax = millis();
      htmlToAtoms(reinterpret_cast<char*>(xml), n, sink, &ir);
      saxMs += millis() - tSax;
      parsed = true;
    }
    free(xml);
    if (!parsed && onDisk) {
      inflateMs += millis() - tInf;
      char* sniffBuf = static_cast<char*>(malloc(4096));
      if (sniffBuf) {
        const int got = hf.read(sniffBuf, 4096);
        if (got > 0) {
          sniffed = takeWritingMode(sniffBuf, static_cast<size_t>(got), mode, chapterMode);
        }
        free(sniffBuf);
      }
      hf.seekSet(0);
      items[i].atomOff = writer.position();
      writeChapterMode(writer, chapterMode);
      auto readHf = [](void* ctx, char* dst, int max) -> int {
        return static_cast<HalFile*>(ctx)->read(dst, static_cast<size_t>(max));
      };
      const unsigned long tSax = millis();
      htmlToAtomsPull(readHf, &hf, sink, &ir);
      saxMs += millis() - tSax;
      parsed = true;
    }
    if (!parsed) {
      LOG_ERR("EPUB", "parse %s failed: %s", ent->name, zip.lastError());
      items[i].done = 1;
      sliceEnd = static_cast<uint16_t>(i + 1);
      continue;
    }
    LOG_INF("EPUB", "ch %u %s %uB chars=%u", i, ent->name, static_cast<unsigned>(ent->uncompSize),
            static_cast<unsigned>(ctx.ch));
    ++htmlOk;
    if (ir.title[0] && items[i].title[0] == 0) {
      snprintf(items[i].title, sizeof(items[i].title), "%s", ir.title);
    }
    items[i].compact = ir.compactColumns ? 1 : 0;
    items[i].hasMode = (sniffed || ir.hasMode) ? 1 : 0;
    items[i].mode = chapterMode;
    items[i].chCount = ctx.ch;
    if (!writer.ok()) {
      break;
    }
    items[i].done = 1;
    sliceEnd = static_cast<uint16_t>(i + 1);
    if (ctx.ch == 0) {
      continue;
    }
    Atom gap{};
    gap.kind = AtomKind::PageBreak;
    writer.write(gap);
  }
  if (!writer.ok()) {
    error = writer.diskFull() ? "disk full" : "atom write";
    const uint32_t at = writer.position();
    writer.close();
    if (appendAtoms && atomKeep >= 4) {
      HalFile torn = Storage.open(atomPath, O_RDWR);
      if (torn) {
        torn.truncate(atomKeep);
        torn.close();
      }
    } else if (atomPath && atomPath[0]) {
      // A failed first slice has no chapter sidecar. Drop it so it does not hold the free space.
      Storage.remove(atomPath);
    }
    LOG_ERR("EPUB", "%s at %lu", error, static_cast<unsigned long>(at));
    close();
    return false;
  }
  if (!tocLoaded && !loadTocDoc(opfDir)) {
    LOG_INF("EPUB", "No NCX/nav TOC");
  }
  for (uint16_t i = begin; i < sliceEnd; ++i) {
    items[i].done = 1;
  }
  bindTocToSpine();
  report(sliceEnd, nSpine);
  const uint32_t atomBytes = writer.position() - atomStart;
  writer.close();
  const uint16_t nPic = pics.count();
  pics.end();
  error = "";
  LOG_INF("EPUB", "'%s' spine=%u/%u html=%u pics=%u atoms=%u infl %lums sax %lums total %lums", bookTitle, sliceEnd,
          nSpine, htmlOk, nPic, static_cast<unsigned>(atomBytes), inflateMs, saxMs, millis() - t0);
  if (wholeBook && ((htmlOk == 0 && nPic == 0) || (atomBytes < 64 && nPic == 0))) {
    error = "no html";
    return false;
  }
  if (!wholeBook && sliceEnd == begin && begin < nSpine) {
    error = "no html";
    return false;
  }
  // The caller reloads the font cmap next. The zip table and the spine are
  // the blocks that used to make that allocation fail.
  free(items);
  items = nullptr;
  zip.close();
  return true;
}

void EpubBook::close() {
  zip.close();
  free(items);
  items = nullptr;
  free(tocs);
  tocs = nullptr;
  nSpine = 0;
  nToc = 0;
  tocCap = 0;
  ncxHref[0] = 0;
  navHref[0] = 0;
  bookTitle[0] = 0;
  bookAuthor[0] = 0;
}

bool EpubBook::parseContainer(char* opfName, const size_t cap) {
  const ZipArchive::Entry* e = zip.find("META-INF/container.xml");
  if (!e) {
    error = "no container";
    return false;
  }
  LOG_INF("EPUB", "container %s %u bytes", e->name, static_cast<unsigned>(e->uncompSize));
  size_t n = 0;
  uint8_t* xml = zip.extract(*e, &n);
  if (!xml) {
    error = zip.lastError();
    return false;
  }
  HtmlSax sax;
  sax.bind(reinterpret_cast<char*>(xml), n);
  bool found = false;
  while (sax.next()) {
    if ((sax.kind == HtmlSax::Kind::Start || sax.kind == HtmlSax::Kind::Empty) && strcmp(sax.name, "rootfile") == 0) {
      if (sax.attr("full-path", opfName, cap) && opfName[0]) {
        found = true;
        break;
      }
    }
  }
  free(xml);
  if (!found) {
    error = "no rootfile";
    return false;
  }
  return true;
}

bool EpubBook::parseOpf(bool (*rewind)(void*, HtmlSax*), void* ctx, const char* opfDir) {
  if (!rewind) {
    error = "opf read";
    return false;
  }
  HtmlSax sax;
  if (!rewind(ctx, &sax)) {
    error = "opf read";
    return false;
  }

  // The spine block is taken first so the smaller id table sits at the tail.
  // Freeing the ids then leaves one hole big enough for the 32KB deflate window.
  items = static_cast<Spine*>(calloc(kMaxSpine, sizeof(Spine)));
  auto* spineIds = static_cast<char(*)[40]>(calloc(kMaxSpine, 40));
  if (!items || !spineIds) {
    free(items);
    items = nullptr;
    free(spineIds);
    error = "opf oom";
    return false;
  }

  uint16_t nRef = 0;
  bool capped = false;
  bool sawItem = false;
  bool inManifest = false;
  bool inSpine = false;
  char tocId[40]{};
  char taking[16]{};
  while (sax.next()) {
    if (sax.kind == HtmlSax::Kind::Start || sax.kind == HtmlSax::Kind::Empty) {
      if (sax.kind == HtmlSax::Kind::Start) {
        if (strcmp(sax.name, "title") == 0) {
          snprintf(taking, sizeof(taking), "title");
        } else if (strcmp(sax.name, "creator") == 0) {
          snprintf(taking, sizeof(taking), "creator");
        } else {
          taking[0] = 0;
        }
      }
      if (strcmp(sax.name, "manifest") == 0) {
        inManifest = true;
      } else if (strcmp(sax.name, "spine") == 0) {
        inSpine = true;
        sax.attr("toc", tocId, sizeof(tocId));
      } else if (inManifest && strcmp(sax.name, "item") == 0) {
        sawItem = true;
      } else if (inSpine && strcmp(sax.name, "itemref") == 0) {
        char id[40]{};
        sax.attr("idref", id, sizeof(id));
        if (id[0] == 0) {
          continue;
        }
        if (nRef < kMaxSpine) {
          snprintf(spineIds[nRef], sizeof(spineIds[nRef]), "%s", id);
          ++nRef;
        } else if (!capped) {
          capped = true;
          LOG_INF("EPUB", "spine capped at %u", static_cast<unsigned>(kMaxSpine));
        }
      } else if (strcmp(sax.name, "meta") == 0) {
        char name[48]{};
        char prop[48]{};
        char content[48]{};
        sax.attr("name", name, sizeof(name));
        sax.attr("property", prop, sizeof(prop));
        sax.attr("content", content, sizeof(content));
        if (strstr(name, "writing-mode") || strstr(prop, "writing-mode") || strcmp(name, "primary-writing-mode") == 0) {
          if (strstr(content, "horizontal") || strstr(content, "lr-tb")) {
            mode = WritingMode::HorizontalTb;
          } else if (strstr(content, "vertical") || strstr(content, "tb-rl")) {
            mode = WritingMode::VerticalRl;
          }
        }
      }
    } else if (sax.kind == HtmlSax::Kind::End) {
      taking[0] = 0;
      if (strcmp(sax.name, "manifest") == 0) {
        inManifest = false;
      } else if (strcmp(sax.name, "spine") == 0) {
        inSpine = false;
      }
    } else if (sax.kind == HtmlSax::Kind::Text) {
      if (strcmp(taking, "title") == 0 && bookTitle[0] == 0) {
        snprintf(bookTitle, sizeof(bookTitle), "%s", sax.text);
      } else if (strcmp(taking, "creator") == 0 && bookAuthor[0] == 0) {
        snprintf(bookAuthor, sizeof(bookAuthor), "%s", sax.text);
      }
    }
  }
  if (!sawItem || nRef == 0) {
    free(items);
    items = nullptr;
    free(spineIds);
    error = "empty spine";
    return false;
  }
  for (uint16_t s = 0; s < nRef; ++s) {
    items[s].zipIndex = 0xFFFFu;
  }
  if (!rewind(ctx, &sax)) {
    free(items);
    items = nullptr;
    free(spineIds);
    error = "opf read";
    return false;
  }

  inManifest = false;
  while (sax.next()) {
    if (sax.kind == HtmlSax::Kind::Start || sax.kind == HtmlSax::Kind::Empty) {
      if (strcmp(sax.name, "manifest") == 0) {
        inManifest = true;
      } else if (inManifest && strcmp(sax.name, "item") == 0) {
        char id[40]{};
        char href[96]{};
        char type[40]{};
        sax.attr("id", id, sizeof(id));
        sax.attr("href", href, sizeof(href));
        sax.attr("media-type", type, sizeof(type));
        char props[80]{};
        const bool nav = sax.attr("properties", props, sizeof(props)) && strstr(props, "nav") != nullptr;
        if (href[0] && tocId[0] && strcmp(id, tocId) == 0) {
          snprintf(ncxHref, sizeof(ncxHref), "%s", href);
        } else if (href[0] && isNcxType(type) && ncxHref[0] == 0) {
          snprintf(ncxHref, sizeof(ncxHref), "%s", href);
        }
        if (href[0] && nav && navHref[0] == 0) {
          snprintf(navHref, sizeof(navHref), "%s", href);
        }
        if (id[0] == 0 || href[0] == 0) {
          continue;
        }
        uint8_t kind = Spine::kOther;
        if (isSvgType(type)) {
          kind = Spine::kSvg;
        } else if (isImageType(type)) {
          kind = Spine::kImage;
        } else if (isHtmlType(type)) {
          kind = Spine::kHtml;
        }
        bool wanted = false;
        for (uint16_t s = 0; s < nRef; ++s) {
          if (strcmp(spineIds[s], id) == 0 && items[s].zipIndex == 0xFFFFu) {
            wanted = true;
            break;
          }
        }
        if (!wanted) {
          continue;
        }
        const ZipArchive::Entry* ent = nullptr;
        if (kind == Spine::kImage || kind == Spine::kSvg) {
          char full[128];
          joinPath(full, sizeof(full), opfDir, href);
          ent = findImage(zip, "", full);
          if (!ent) {
            ent = zip.find(href);
          }
        } else {
          ent = findSpineFile(zip, opfDir, href);
        }
        const uint16_t zid = zip.indexOf(ent);
        if (zid == 0xFFFFu) {
          LOG_ERR("EPUB", "spine file %s", href);
          continue;
        }
        for (uint16_t s = 0; s < nRef; ++s) {
          if (strcmp(spineIds[s], id) != 0 || items[s].zipIndex != 0xFFFFu) {
            continue;
          }
          items[s].zipIndex = zid;
          items[s].kind = kind;
        }
      }
    } else if (sax.kind == HtmlSax::Kind::End && strcmp(sax.name, "manifest") == 0) {
      inManifest = false;
    }
  }
  free(spineIds);

  uint16_t w = 0;
  for (uint16_t i = 0; i < nRef; ++i) {
    if (items[i].zipIndex == 0xFFFFu) {
      continue;
    }
    if (w != i) {
      items[w] = items[i];
    }
    ++w;
  }
  nSpine = w;
  if (nSpine == 0) {
    free(items);
    items = nullptr;
    error = "empty spine";
    return false;
  }
  if (nSpine < kMaxSpine) {
    auto* shrunk = static_cast<Spine*>(realloc(items, static_cast<size_t>(nSpine) * sizeof(Spine)));
    if (shrunk) {
      items = shrunk;
    }
  }
  return true;
}

void EpubBook::addToc(const char* title, const char* href) {
  if (!href || href[0] == 0 || nToc >= kMaxSpine) {
    return;
  }
  if (nToc >= tocCap) {
    const uint16_t cap = tocCap == 0 ? 8 : (tocCap * 2 > kMaxSpine ? kMaxSpine : static_cast<uint16_t>(tocCap * 2));
    if (nToc >= cap) {
      return;
    }
    auto* grown = static_cast<TocEntry*>(realloc(tocs, cap * sizeof(TocEntry)));
    if (!grown) {
      return;
    }
    if (cap > tocCap) {
      memset(grown + tocCap, 0, (cap - tocCap) * sizeof(TocEntry));
    }
    tocs = grown;
    tocCap = cap;
  }
  TocEntry& e = tocs[nToc];
  snprintf(e.href, sizeof(e.href), "%s", href);
  percentDecodeInPlace(e.href);
  stripHash(e.href);
  if (title && title[0]) {
    snprintf(e.title, sizeof(e.title), "%s", title);
    e.title[utf8SafeTruncateBuffer(e.title, static_cast<int>(strlen(e.title)))] = '\0';
  }
  if (e.title[0] == 0) {
    localName(e.title, sizeof(e.title), e.href);
    char* dot = strrchr(e.title, '.');
    if (dot) {
      *dot = 0;
    }
  }
  e.atomOff = 0xFFFFFFFFu;
  e.spine = 0xFFFFu;
  ++nToc;
}

bool EpubBook::parseNcx(HtmlSax& sax) {
  char label[80]{};
  bool inLabel = false;
  bool takeText = false;
  while (sax.next()) {
    if (sax.kind == HtmlSax::Kind::Start || sax.kind == HtmlSax::Kind::Empty) {
      if (strcmp(sax.name, "navpoint") == 0) {
        label[0] = 0;
        inLabel = false;
        takeText = false;
      } else if (strcmp(sax.name, "navlabel") == 0) {
        inLabel = true;
      } else if (strcmp(sax.name, "text") == 0 && inLabel) {
        takeText = true;
      } else if (strcmp(sax.name, "content") == 0) {
        char src[96]{};
        sax.attr("src", src, sizeof(src));
        addToc(label, src);
      }
    } else if (sax.kind == HtmlSax::Kind::End) {
      if (strcmp(sax.name, "navlabel") == 0) {
        inLabel = false;
        takeText = false;
      } else if (strcmp(sax.name, "text") == 0) {
        takeText = false;
      }
    } else if (sax.kind == HtmlSax::Kind::Text && takeText && sax.text[0]) {
      if (label[0] == 0) {
        snprintf(label, sizeof(label), "%s", sax.text);
      }
    }
  }
  return nToc > 0;
}

bool EpubBook::parseNavDoc(HtmlSax& sax) {
  bool inToc = false;
  char pendingHref[96]{};
  bool takeLink = false;
  while (sax.next()) {
    if (sax.kind == HtmlSax::Kind::Start || sax.kind == HtmlSax::Kind::Empty) {
      if (strcmp(sax.name, "nav") == 0) {
        inToc = sax.attrContains("type", "toc") || sax.hasClass("toc");
      } else if (inToc && strcmp(sax.name, "a") == 0) {
        pendingHref[0] = 0;
        sax.attr("href", pendingHref, sizeof(pendingHref));
        takeLink = pendingHref[0] != 0;
      }
    } else if (sax.kind == HtmlSax::Kind::End) {
      if (strcmp(sax.name, "nav") == 0) {
        inToc = false;
      } else if (strcmp(sax.name, "a") == 0) {
        takeLink = false;
        pendingHref[0] = 0;
      }
    } else if (sax.kind == HtmlSax::Kind::Text && takeLink && sax.text[0] && pendingHref[0]) {
      addToc(sax.text, pendingHref);
      takeLink = false;
      pendingHref[0] = 0;
    }
  }
  return nToc > 0;
}

bool EpubBook::loadTocDoc(const char* opfDir) {
  const char* rel = ncxHref[0] ? ncxHref : (navHref[0] ? navHref : nullptr);
  if (!rel) {
    return false;
  }
  char full[128];
  joinPath(full, sizeof(full), opfDir, rel);
  const ZipArchive::Entry* ent = zip.find(full);
  if (!ent) {
    ent = zip.find(rel);
  }
  if (!ent) {
    const char* base = strrchr(rel, '/');
    base = base ? base + 1 : rel;
    ent = zip.findSuffix(base);
  }
  if (!ent) {
    return false;
  }
  uint8_t* xml = nullptr;
  size_t n = 0;
  HalFile tocFile;
  bool tocOnDisk = false;
  if (!openMember(zip, *ent, "/.crossjp/work.toc", xml, n, tocFile, tocOnDisk)) {
    return false;
  }
  XmlCursor tocCur;
  tocCur.mem = reinterpret_cast<const char*>(xml);
  tocCur.memLen = n;
  tocCur.file = tocOnDisk ? &tocFile : nullptr;
  HtmlSax sax;
  const bool bound = rewindXml(&tocCur, &sax);
  const bool ok = bound && (ncxHref[0] ? parseNcx(sax) : parseNavDoc(sax));
  free(xml);
  LOG_INF("EPUB", "TOC %s entries=%u", rel, nToc);
  return ok;
}

void EpubBook::bindTocToSpine() {
  auto spineName = [&](uint16_t i) -> const char* {
    if (items[i].zipIndex == 0xFFFFu || items[i].zipIndex >= zip.count()) {
      return "";
    }
    return zip.at(items[i].zipIndex).name;
  };
  if (nToc == 0) {
    for (uint16_t i = 0; i < nSpine; ++i) {
      if (!items[i].done || items[i].kind != Spine::kHtml) {
        continue;
      }
      const char* name = spineName(i);
      if (items[i].title[0]) {
        addToc(items[i].title, name);
      } else {
        char fallback[24];
        snprintf(fallback, sizeof(fallback), "Chapter %u", static_cast<unsigned>(nToc + 1));
        addToc(fallback, name);
      }
      if (nToc > 0) {
        tocs[nToc - 1].atomOff = items[i].atomOff;
        tocs[nToc - 1].spine = i;
      }
    }
    LOG_INF("EPUB", "TOC from spine entries=%u", nToc);
    return;
  }
  for (uint16_t t = 0; t < nToc; ++t) {
    for (uint16_t i = 0; i < nSpine; ++i) {
      if (!hrefMatch(spineName(i), tocs[t].href)) {
        continue;
      }
      tocs[t].spine = i;
      if (!items[i].done) {
        tocs[t].atomOff = 0xFFFFFFFFu;
        break;
      }
      // 角川/青空 style: TOC points at a title-only XHTML; body is the next file.
      // Only walk spines this slice actually wrote. A later file still has chCount 0.
      uint16_t j = i;
      while (j < nSpine && items[j].done && items[j].chCount < 32) {
        ++j;
      }
      if (j >= nSpine || !items[j].done) {
        j = i;
      }
      tocs[t].atomOff = items[j].atomOff;
      tocs[t].spine = j;
      break;
    }
  }
}

}  // namespace ts
