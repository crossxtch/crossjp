#include "screens/ReaderScreen.h"

#include <Gfx.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <cstdio>
#include <cstring>

#include <algorithm>

#include <HalTiltSensor.h>

#include "core/BookCache.h"
#include "core/Power.h"
#include "core/ReadingFont.h"
#include "core/Settings.h"
#include "core/UiText.h"
#include "core/fontIds.h"
#include "screens/ChapterSelectionScreen.h"

namespace {
void progressPath(char* out, size_t outSize, const char* bookPath) {
  snprintf(out, outSize, "%s/p_%08lx.bin", Settings::kDir, static_cast<unsigned long>(BookCache::key(bookPath)));
}
}  // namespace

ReaderScreen::ReaderScreen(Gfx& gfx, MappedInput& input, const char* path) : Screen("Reader", gfx, input) {
  snprintf(bookPath, sizeof(bookPath), "%s", path ? path : "");
}

uint16_t ReaderScreen::bookPageCount() const { return book ? book->pageCount() : 0; }

void ReaderScreen::clampPage() {
  if (!book || book->indexedSpan() == 0) {
    page = 0;
    return;
  }
  const uint32_t front = book->pagesBefore();
  const uint32_t end = front + book->indexedSpan();
  if (page >= end) {
    page = end - 1;
  }
  if (page < front) {
    page = front;
  }
}

uint32_t ReaderScreen::viewPage() const {
  if (!book || book->indexedSpan() == 0) {
    return 0;
  }
  const uint32_t front = book->pagesBefore();
  if (page < front) {
    return 0;
  }
  const uint32_t local = page - front;
  if (local >= book->indexedSpan()) {
    return static_cast<uint32_t>(book->indexedSpan() - 1);
  }
  return local;
}

const char* ReaderScreen::bookError() const {
  if (book) {
    const char* err = book->lastError();
    if (err && err[0] != '\0') {
      return err;
    }
  }
  return openError;
}

const std::vector<ts::ChapterInfo>& ReaderScreen::bookChapters() { return book->getChapters(); }

XgfFont* ReaderScreen::cjkFont() { return (loaded && book) ? book->cjkFont() : nullptr; }

void ReaderScreen::loadProgress() {
  char p[64];
  progressPath(p, sizeof(p), bookPath);
  HalFile f;
  if (!Storage.openFileForRead("PRG", p, f)) {
    page = 0;
    resumeAtom = 0;
    LOG_INF("RDR", "No progress file, start at 0");
    return;
  }
  uint32_t saved = 0;
  if (f.read(&saved, sizeof(saved)) != static_cast<int>(sizeof(saved))) {
    page = 0;
    resumeAtom = 0;
    LOG_ERR("RDR", "Progress file %s unreadable", p);
    return;
  }
  page = saved;
  uint32_t atom = 0;
  if (f.read(&atom, sizeof(atom)) == static_cast<int>(sizeof(atom))) {
    resumeAtom = atom;
  } else {
    resumeAtom = 0;
  }
  LOG_INF("RDR", "Progress %s -> page %lu atom %lu", p, static_cast<unsigned long>(page),
          static_cast<unsigned long>(resumeAtom));
}

void ReaderScreen::saveProgress() const {
  Storage.ensureDirectoryExists(Settings::kDir);
  char p[64];
  progressPath(p, sizeof(p), bookPath);
  HalFile f;
  if (!Storage.openFileForWrite("PRG", p, f)) {
    LOG_ERR("PRG", "Could not write %s", p);
    return;
  }
  const uint32_t atom = book ? book->atomForPage(page) : 0;
  f.write(&page, sizeof(page));
  f.write(&atom, sizeof(atom));
}

void ReaderScreen::onEnter() {
  Screen::onEnter();
  pagesUntilFull = settings.refreshEveryNPages;
  lastOpenProgressMs = 0;
  painted = false;
  loadProgress();
  bool ok = false;
  book = makeUniqueNoThrow<TypesetBook>();
  if (!book) {
    openError = "out of memory";
    LOG_ERR("RDR", "OOM: typeset");
  } else {
    char fontPath[192];
    if (!ReadingFont::activePath(fontPath, sizeof(fontPath))) {
      fontPath[0] = '\0';
    }
    ok = book->open(bookPath, &ReaderScreen::onOpenProgress, this, fontPath[0] ? fontPath : nullptr, page, resumeAtom);
    if (!ok) {
      openError = book->lastError();
    }
  }
  if (!ok) {
    LOG_ERR("RDR", "Failed to open %s: %s", bookPath, bookError());
    loaded = false;
    requestUpdate();
    return;
  }
  loaded = true;
  if (book->openedPartial()) {
    page = book->showAt();
  } else if (page >= bookPageCount()) {
    LOG_INF("RDR", "Saved page %lu past end (%u), clamping", static_cast<unsigned long>(page), bookPageCount());
    page = bookPageCount() > 0 ? bookPageCount() - 1 : 0;
  }
  clampPage();
  snprintf(settings.lastBookPath, sizeof(settings.lastBookPath), "%s", bookPath);
  settings.save();
  LOG_INF("RDR", "Open %s page %lu/%u '%s'", bookPath, static_cast<unsigned long>(page + 1), bookPageCount(),
          book->title());
  requestUpdate();
}

void ReaderScreen::onExit() {
  if (loaded) {
    saveProgress();
  }
  if (book) {
    book->close();
    book.reset();
  }
  halTiltSensor.clearPendingEvents();
  Screen::onExit();
}

void ReaderScreen::onResume() { pagesUntilFull = 1; }

void ReaderScreen::loop() {
  if (!loaded) {
    if (input.wasReleased(MappedInput::Button::Back) || input.wasReleased(MappedInput::Button::Confirm)) {
      finish();
    }
    return;
  }

  // Opportunistic: finish any deferred post-page-render RAM cleanup here,
  // before checking input, so it lands in idle time rather than the next
  // page's blocking render.
  if (book) {
    book->flushPendingCleanup(gfx);
  }

  if (input.wasReleased(MappedInput::Button::Back)) {
    finish();
    return;
  }
  if (input.wasReleased(MappedInput::Button::Confirm)) {
    pagesUntilFull = 1;
    auto screen = makeUniqueNoThrow<ChapterSelectionScreen>(gfx, input, *this, bookChapters(), page, bookPageCount());
    if (!screen) {
      LOG_ERR("RDR", "OOM: chapters");
      return;
    }
    push(std::move(screen));
    return;
  }

  bool moved = false;
  const int delta = input.consumeReaderPageDelta();

  if (delta > 0) {
    const uint32_t maxForward = bookPageCount() > 0 ? bookPageCount() - 1 - page : 0;
    const uint32_t step = std::min(static_cast<uint32_t>(delta), maxForward);
    if (step > 0) {
      const uint32_t target = page + step;
      const uint32_t front = book->pagesBefore();
      const uint32_t indexedEnd = front + book->indexedSpan();
      if (target < front || target >= indexedEnd) {
        const uint32_t atom = book->atomForPage(page);
        const uint16_t frontBefore = book->pagesBefore();
        lastOpenProgressMs = 0;
        if (book->ensureGlobal(target, &ReaderScreen::onOpenProgress, this)) {
          if (book->pagesBefore() != frontBefore) {
            const uint32_t here = book->globalForAtom(atom);
            page = here;
          } else {
            page = target;
          }
          clampPage();
          moved = true;
        }
      } else {
        page = target;
        moved = true;
      }
    } else {
      LOG_DBG("RDR", "Already last page");
    }
  } else if (delta < 0) {
    const bool atFront = page <= book->pagesBefore() && book->pagesBefore() > 0;
    const uint32_t step = std::min(static_cast<uint32_t>(-delta), page);
    if (step > 0 || atFront) {
      const uint32_t target = step > 0 && page >= step ? page - step : 0;
      const uint32_t front = book->pagesBefore();
      if (target < front) {
        const uint32_t atom = book->atomForPage(page);
        const uint16_t frontBefore = book->pagesBefore();
        lastOpenProgressMs = 0;
        if (book->ensureGlobal(target, &ReaderScreen::onOpenProgress, this)) {
          if (book->pagesBefore() != frontBefore) {
            const uint32_t here = book->globalForAtom(atom);
            page = here > step ? here - step : 0;
          } else {
            page = target;
          }
          clampPage();
          moved = true;
        }
      } else {
        page = target;
        moved = true;
      }
    } else {
      LOG_DBG("RDR", "Already first page");
    }
  }
  if (moved) {
    LOG_DBG("RDR", "Page %lu/%u", static_cast<unsigned long>(page + 1), bookPageCount());
    saveProgress();
    requestUpdate();
  } else if (painted && input.queuedPageDelta() == 0 && book) {
    book->extendAhead(page, 5);
  }
}

void ReaderScreen::showStatus(const char* title, const char* detail) {
  gfx.clear(false);
  gfx.drawCenteredText(FONT_UI_BOLD, gfx.height() / 2 - 20, title);
  if (detail && detail[0] != '\0') {
    gfx.drawCenteredText(FONT_UI, gfx.height() / 2 + 10, detail);
  }
  gfx.present(HalDisplay::HALF_REFRESH);
}

void ReaderScreen::onOpenProgress(void* ctx, const uint16_t done, const uint16_t total) {
  static_cast<ReaderScreen*>(ctx)->showOpenProgress(done, total);
}

void ReaderScreen::showOpenProgress(const uint16_t done, const uint16_t total) {
  yield();
  constexpr unsigned long kMinMs = 4000;
  const unsigned long now = millis();
  const bool first = lastOpenProgressMs == 0;
  const bool last = total > 0 && done >= total;
  if (!first && !last && now - lastOpenProgressMs < kMinMs) {
    return;
  }
  lastOpenProgressMs = now;

  gfx.clear(false);
  gfx.drawCenteredText(FONT_UI_BOLD, gfx.height() / 2 - 36, uiText::opening);
  const int barW = gfx.width() - 80;
  const int barH = 18;
  const int barX = 40;
  const int barY = gfx.height() / 2;
  gfx.fillRect(barX, barY, barW, barH, true);
  gfx.fillRect(barX + 2, barY + 2, barW - 4, barH - 4, false);
  if (total > 0 && done > 0) {
    uint32_t fill = (static_cast<uint32_t>(barW - 4) * done) / total;
    if (fill > static_cast<uint32_t>(barW - 4)) {
      fill = static_cast<uint32_t>(barW - 4);
    }
    if (fill > 0) {
      gfx.fillRect(barX + 2, barY + 2, static_cast<int>(fill), barH - 4, true);
    }
  }
  if (total > 0) {
    char line[24];
    snprintf(line, sizeof(line), "%u / %u", static_cast<unsigned>(done), static_cast<unsigned>(total));
    gfx.drawCenteredText(FONT_UI, barY + barH + 16, line);
  }
  gfx.present(first ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
}

void ReaderScreen::jumpToPage(const uint32_t targetPage) {
  if (!loaded || !book || bookPageCount() == 0) {
    return;
  }
  uint32_t target = targetPage;
  if (target >= bookPageCount()) {
    target = bookPageCount() - 1;
  }
  const uint32_t front = book->pagesBefore();
  const uint32_t indexedEnd = front + book->indexedSpan();
  if (target < front || target >= indexedEnd) {
    lastOpenProgressMs = 0;
    if (!book->ensureGlobal(target, &ReaderScreen::onOpenProgress, this)) {
      LOG_ERR("RDR", "Page %lu not ready: %s", static_cast<unsigned long>(target + 1), bookError());
    }
  }
  page = target;
  clampPage();
  pagesUntilFull = 1;
  LOG_INF("RDR", "Jumped to page %lu/%u", static_cast<unsigned long>(page + 1), bookPageCount());
  saveProgress();
  requestUpdate();
}

void ReaderScreen::openChapter(const uint16_t chapterIndex) {
  if (!loaded || !book) {
    return;
  }
  uint32_t target = 0;
  lastOpenProgressMs = 0;
  if (!book->ensureChapter(chapterIndex, target, &ReaderScreen::onOpenProgress, this)) {
    LOG_ERR("RDR", "Chapter %u not ready: %s", static_cast<unsigned>(chapterIndex), bookError());
    return;
  }
  page = target;
  clampPage();
  pagesUntilFull = 1;
  LOG_INF("RDR", "Chapter %u page %lu/%u", static_cast<unsigned>(chapterIndex), static_cast<unsigned long>(page + 1),
          bookPageCount());
  saveProgress();
  requestUpdate();
}

void ReaderScreen::render() {
  if (!loaded) {
    showStatus(uiText::couldNotOpenBook, uiText::error(bookError()));
    return;
  }

  const unsigned long blitStart = millis();
  const uint32_t local = viewPage();
  const bool drew = book && book->drawPage(gfx, local, pagesUntilFull, settings.refreshEveryNPages);
  if (!drew) {
    LOG_ERR("RDR", "Blit page %lu failed: %s", static_cast<unsigned long>(page), bookError());
    showStatus(uiText::error(bookError()));
    return;
  }
  painted = true;
  const unsigned long blitMs = millis() - blitStart;
  LOG_DBG("RDR", "Blit page %lu/%u %lums", static_cast<unsigned long>(page + 1), bookPageCount(), blitMs);
  if (power::tiltLocked()) {
    power::paintGyroOffMarker();
  }

  // Skip prefetch when the user already queued a skip-ahead or back-turn
  // during the blit; loading N+1 would delay that jump by ~146 ms.
  const int queued = input.queuedPageDelta();
  if (queued >= 0 && queued <= 1) {
    if (book) {
      book->prefetchForward(local);
    }
  } else {
    LOG_DBG("RDR", "Skip prefetch (queued delta %d)", queued);
  }
}
