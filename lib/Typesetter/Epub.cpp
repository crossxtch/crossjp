#include "Epub.h"

#include "HtmlSax.h"

#include <Logging.h>
#include <Utf8.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ts {
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

struct ManifestItem {
  char id[40]{};
  char href[96]{};
  char type[40]{};
  bool nav = false;
};

}  // namespace

bool EpubBook::open(const char* epubPath, const char* atomPath, ProgressFn progress, void* progressCtx) {
  close();
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
  size_t opfLen = 0;
  uint8_t* opf = zip.extract(*opfEnt, &opfLen);
  if (!opf) {
    error = zip.lastError();
    close();
    return false;
  }
  const bool okOpf = parseOpf(reinterpret_cast<char*>(opf), opfLen);
  free(opf);
  if (!okOpf) {
    close();
    return false;
  }

  char opfDir[96]{};
  dirnameOf(opfDir, sizeof(opfDir), opfName);
  if (!loadTocDoc(opfDir)) {
    LOG_INF("EPUB", "No NCX/nav TOC");
  }

  AtomWriter writer;
  if (!writer.open(atomPath)) {
    error = "atom file";
    close();
    return false;
  }

  struct Ctx {
    AtomWriter* w;
    bool ok;
    uint16_t ch;
  } ctx{&writer, true, 0};
  AtomSink sink{};
  sink.ctx = &ctx;
  sink.emit = [](void* c, const Atom& a, uint32_t) -> bool {
    auto* x = static_cast<Ctx*>(c);
    if (a.kind == AtomKind::Ch || a.kind == AtomKind::Tcy || a.kind == AtomKind::Group) {
      if (x->ch < 0xFFFFu) {
        ++x->ch;
      }
    }
    if (!x->w->write(a)) {
      x->ok = false;
      return false;
    }
    return true;
  };

  uint16_t htmlOk = 0;
  uint32_t atomStart = writer.position();
  unsigned long inflateMs = 0;
  unsigned long saxMs = 0;
  for (uint16_t i = 0; i < nSpine; ++i) {
    report(i, nSpine);
    items[i].atomOff = writer.position();
    ctx.ch = 0;
    if (isImageType(items[i].type)) {
      Atom pb{};
      pb.kind = AtomKind::PageBreak;
      writer.write(pb);
      continue;
    }
    if (!isHtmlType(items[i].type)) {
      LOG_INF("EPUB", "skip %s (%s)", items[i].href, items[i].type);
      continue;
    }
    char full[128];
    joinPath(full, sizeof(full), opfDir, items[i].href);
    const ZipArchive::Entry* ent = zip.find(full);
    if (!ent) {
      ent = zip.find(items[i].href);
    }
    if (!ent) {
      const char* base = strrchr(items[i].href, '/');
      base = base ? base + 1 : items[i].href;
      ent = zip.findSuffix(base);
    }
    if (!ent) {
      LOG_ERR("EPUB", "spine missing %s", items[i].href);
      continue;
    }
    HtmlIRResult ir{};
    const unsigned long tInf = millis();
    bool parsed = false;
    WritingMode chapterMode = mode;
    bool sniffed = false;
    size_t n = 0;
    uint8_t* xml = zip.extract(*ent, &n);
    if (xml && n > 0) {
      inflateMs += millis() - tInf;
      WritingMode found = mode;
      sniffed = sniffWritingMode(reinterpret_cast<char*>(xml), n, found);
      if (sniffed) {
        chapterMode = found;
      }
      items[i].atomOff = writer.position();
      Atom modeAtom{};
      modeAtom.kind = AtomKind::Mode;
      modeAtom.cp = chapterMode == WritingMode::HorizontalTb ? 1u : 0u;
      writer.write(modeAtom);
      const unsigned long tSax = millis();
      htmlToAtoms(reinterpret_cast<char*>(xml), n, sink, &ir);
      saxMs += millis() - tSax;
      parsed = true;
    }
    free(xml);
    if (!parsed) {
      const char* tmp = "/.crossjp/work.xhtml";
      if (!zip.extractToFile(*ent, tmp)) {
        LOG_ERR("EPUB", "extract %s (%u bytes): %s", items[i].href, static_cast<unsigned>(ent->uncompSize),
                zip.lastError());
        continue;
      }
      inflateMs += millis() - tInf;
      HalFile hf;
      if (!Storage.openFileForRead("EPUB", tmp, hf) || hf.fileSize() == 0) {
        LOG_ERR("EPUB", "open work %s", items[i].href);
        continue;
      }
      hf.probeContiguous();
      char* sniffBuf = static_cast<char*>(malloc(4096));
      if (sniffBuf) {
        const int got = hf.read(sniffBuf, 4096);
        if (got > 0) {
          WritingMode found = mode;
          sniffed = sniffWritingMode(sniffBuf, static_cast<size_t>(got), found);
          if (sniffed) {
            chapterMode = found;
          }
        }
        free(sniffBuf);
      }
      hf.seekSet(0);
      items[i].atomOff = writer.position();
      Atom modeAtom{};
      modeAtom.kind = AtomKind::Mode;
      modeAtom.cp = chapterMode == WritingMode::HorizontalTb ? 1u : 0u;
      writer.write(modeAtom);
      auto readHf = [](void* ctx, char* dst, int max) -> int {
        return static_cast<HalFile*>(ctx)->read(dst, static_cast<size_t>(max));
      };
      const unsigned long tSax = millis();
      htmlToAtomsPull(readHf, &hf, sink, &ir);
      saxMs += millis() - tSax;
      parsed = true;
    }
    if (!parsed) {
      LOG_ERR("EPUB", "parse %s failed: %s", items[i].href, zip.lastError());
      continue;
    }
    LOG_INF("EPUB", "ch %u %s %uB chars=%u", i, items[i].href, static_cast<unsigned>(ent->uncompSize),
            static_cast<unsigned>(ctx.ch));
    ++htmlOk;
    if (ir.title[0] && items[i].title[0] == 0) {
      snprintf(items[i].title, sizeof(items[i].title), "%s", ir.title);
    }
    items[i].compact = ir.compactColumns ? 1 : 0;
    items[i].hasMode = (sniffed || ir.hasMode) ? 1 : 0;
    items[i].mode = chapterMode;
    items[i].chCount = ctx.ch;
    if (!ctx.ok) {
      error = "atom write";
      writer.close();
      close();
      return false;
    }
    if (ctx.ch == 0) {
      continue;
    }
    Atom gap{};
    gap.kind = AtomKind::PageBreak;
    writer.write(gap);
  }
  bindTocToSpine();
  report(nSpine, nSpine);
  const uint32_t atomBytes = writer.position() - atomStart;
  writer.close();
  error = "";
  LOG_INF("EPUB", "'%s' spine=%u html=%u atoms=%u infl %lums sax %lums total %lums", bookTitle, nSpine, htmlOk,
          static_cast<unsigned>(atomBytes), inflateMs, saxMs, millis() - t0);
  if (htmlOk == 0 || atomBytes < 64) {
    error = "no html";
    return false;
  }
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

bool EpubBook::parseOpf(const char* xml, const size_t n) {
  HtmlSax sax;
  sax.bind(xml, n);
  uint16_t nMan = 0;
  uint16_t nRef = 0;
  bool inManifest = false;
  bool inSpine = false;
  while (sax.next()) {
    if (sax.kind == HtmlSax::Kind::Start || sax.kind == HtmlSax::Kind::Empty) {
      if (strcmp(sax.name, "manifest") == 0) {
        inManifest = true;
      } else if (strcmp(sax.name, "spine") == 0) {
        inSpine = true;
      } else if (inManifest && strcmp(sax.name, "item") == 0) {
        if (nMan < kMaxSpine) {
          ++nMan;
        }
      } else if (inSpine && strcmp(sax.name, "itemref") == 0) {
        if (nRef < kMaxSpine) {
          ++nRef;
        }
      }
    } else if (sax.kind == HtmlSax::Kind::End) {
      if (strcmp(sax.name, "manifest") == 0) {
        inManifest = false;
      } else if (strcmp(sax.name, "spine") == 0) {
        inSpine = false;
      }
    }
  }
  if (nMan == 0 || nRef == 0) {
    error = "empty spine";
    return false;
  }

  auto* man = static_cast<ManifestItem*>(calloc(nMan, sizeof(ManifestItem)));
  auto* spineIds = static_cast<char(*)[40]>(calloc(nRef, 40));
  if (!man || !spineIds) {
    free(man);
    free(spineIds);
    error = "opf oom";
    return false;
  }

  uint16_t iMan = 0;
  uint16_t iRef = 0;
  char tocId[40]{};
  sax.bind(xml, n);
  inManifest = false;
  inSpine = false;
  while (sax.next()) {
    if (sax.kind == HtmlSax::Kind::Start || sax.kind == HtmlSax::Kind::Empty) {
      if (strcmp(sax.name, "manifest") == 0) {
        inManifest = true;
      } else if (strcmp(sax.name, "spine") == 0) {
        inSpine = true;
        sax.attr("toc", tocId, sizeof(tocId));
      } else if (inManifest && strcmp(sax.name, "item") == 0 && iMan < nMan) {
        ManifestItem& it = man[iMan];
        sax.attr("id", it.id, sizeof(it.id));
        sax.attr("href", it.href, sizeof(it.href));
        sax.attr("media-type", it.type, sizeof(it.type));
        char props[80];
        if (sax.attr("properties", props, sizeof(props)) && strstr(props, "nav")) {
          it.nav = true;
        }
        if (isNcxType(it.type) || (tocId[0] && strcmp(it.id, tocId) == 0)) {
          snprintf(ncxHref, sizeof(ncxHref), "%s", it.href);
        }
        if (it.nav && navHref[0] == 0) {
          snprintf(navHref, sizeof(navHref), "%s", it.href);
        }
        ++iMan;
      } else if (inSpine && strcmp(sax.name, "itemref") == 0 && iRef < nRef) {
        sax.attr("idref", spineIds[iRef], sizeof(spineIds[iRef]));
        if (spineIds[iRef][0]) {
          ++iRef;
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
      if (strcmp(sax.name, "manifest") == 0) {
        inManifest = false;
      } else if (strcmp(sax.name, "spine") == 0) {
        inSpine = false;
      }
    }
  }

  // second pass for dc:title / creator text — simple scan
  sax.bind(xml, n);
  char taking[16]{};
  while (sax.next()) {
    if (sax.kind == HtmlSax::Kind::Start) {
      if (strcmp(sax.name, "title") == 0) {
        snprintf(taking, sizeof(taking), "title");
      } else if (strcmp(sax.name, "creator") == 0) {
        snprintf(taking, sizeof(taking), "creator");
      } else {
        taking[0] = 0;
      }
    } else if (sax.kind == HtmlSax::Kind::End) {
      taking[0] = 0;
    } else if (sax.kind == HtmlSax::Kind::Text) {
      if (strcmp(taking, "title") == 0 && bookTitle[0] == 0) {
        snprintf(bookTitle, sizeof(bookTitle), "%s", sax.text);
      } else if (strcmp(taking, "creator") == 0 && bookAuthor[0] == 0) {
        snprintf(bookAuthor, sizeof(bookAuthor), "%s", sax.text);
      }
    }
  }

  nRef = iRef;
  nMan = iMan;
  items = static_cast<Spine*>(calloc(nRef ? nRef : 1, sizeof(Spine)));
  if (!items) {
    free(man);
    free(spineIds);
    error = "spine oom";
    return false;
  }
  nSpine = 0;
  for (uint16_t i = 0; i < nRef && nSpine < kMaxSpine; ++i) {
    const ManifestItem* found = nullptr;
    for (uint16_t m = 0; m < nMan; ++m) {
      if (strcmp(man[m].id, spineIds[i]) == 0) {
        found = &man[m];
        break;
      }
    }
    if (!found || !found->href[0]) {
      continue;
    }
    snprintf(items[nSpine].href, sizeof(items[nSpine].href), "%s", found->href);
    percentDecodeInPlace(items[nSpine].href);
    snprintf(items[nSpine].type, sizeof(items[nSpine].type), "%s", found->type);
    ++nSpine;
  }
  if (ncxHref[0] == 0 && tocId[0]) {
    for (uint16_t m = 0; m < nMan; ++m) {
      if (strcmp(man[m].id, tocId) == 0) {
        snprintf(ncxHref, sizeof(ncxHref), "%s", man[m].href);
        break;
      }
    }
  }
  (void)localName;
  free(man);
  free(spineIds);
  if (nSpine == 0) {
    error = "empty spine";
    return false;
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
  ++nToc;
}

bool EpubBook::parseNcx(const char* xml, const size_t n) {
  HtmlSax sax;
  sax.bind(xml, n);
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

bool EpubBook::parseNavDoc(const char* xml, const size_t n) {
  HtmlSax sax;
  sax.bind(xml, n);
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
  size_t n = 0;
  uint8_t* xml = zip.extract(*ent, &n);
  if (!xml) {
    return false;
  }
  const bool ok = ncxHref[0] ? parseNcx(reinterpret_cast<char*>(xml), n)
                             : parseNavDoc(reinterpret_cast<char*>(xml), n);
  free(xml);
  LOG_INF("EPUB", "TOC %s entries=%u", rel, nToc);
  return ok;
}

void EpubBook::bindTocToSpine() {
  if (nToc == 0) {
    for (uint16_t i = 0; i < nSpine; ++i) {
      if (!isHtmlType(items[i].type) || isImageType(items[i].type)) {
        continue;
      }
      if (items[i].title[0]) {
        addToc(items[i].title, items[i].href);
      } else {
        char fallback[24];
        snprintf(fallback, sizeof(fallback), "Chapter %u", static_cast<unsigned>(nToc + 1));
        addToc(fallback, items[i].href);
      }
      if (nToc > 0) {
        tocs[nToc - 1].atomOff = items[i].atomOff;
      }
    }
    LOG_INF("EPUB", "TOC from spine entries=%u", nToc);
    return;
  }
  for (uint16_t t = 0; t < nToc; ++t) {
    for (uint16_t i = 0; i < nSpine; ++i) {
      if (!hrefMatch(items[i].href, tocs[t].href)) {
        continue;
      }
      // 角川/青空 style: TOC points at a title-only XHTML; body is the next file.
      uint16_t j = i;
      while (j < nSpine && items[j].chCount < 32) {
        ++j;
      }
      tocs[t].atomOff = (j < nSpine) ? items[j].atomOff : items[i].atomOff;
      break;
    }
  }
}

}  // namespace ts
