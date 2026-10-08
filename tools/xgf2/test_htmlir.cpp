// c++ -std=c++20 -I lib/Typesetter -I lib/Utf8 \
//   tools/xgf2/test_htmlir.cpp lib/Typesetter/HtmlSax.cpp lib/Typesetter/HtmlIR.cpp \
//   lib/Utf8/Utf8.cpp -o /tmp/test_htmlir && /tmp/test_htmlir

#include "HtmlIR.h"

#include <Utf8.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using ts::Atom;
using ts::AtomKind;

struct Dump {
  std::vector<Atom> atoms;
};

static bool collect(void* ctx, const Atom& a, uint32_t) {
  static_cast<Dump*>(ctx)->atoms.push_back(a);
  return true;
}

static std::vector<Atom> parse(const char* xml) {
  std::string wrapped = xml;
  if (wrapped.find("<html") == std::string::npos) {
    wrapped = std::string("<html xmlns=\"http://www.w3.org/1999/xhtml\"><body>") + xml + "</body></html>";
  }
  Dump d;
  ts::AtomSink sink{&d, collect};
  ts::htmlToAtoms(wrapped.c_str(), wrapped.size(), sink, nullptr);
  // drop leading/trailing gaps like lazahata trimGaps
  while (!d.atoms.empty() && (d.atoms.front().kind == AtomKind::Gap || d.atoms.front().kind == AtomKind::Space ||
                              d.atoms.front().kind == AtomKind::ColumnBreak)) {
    d.atoms.erase(d.atoms.begin());
  }
  while (!d.atoms.empty() && (d.atoms.back().kind == AtomKind::Gap || d.atoms.back().kind == AtomKind::Space ||
                              d.atoms.back().kind == AtomKind::ColumnBreak)) {
    d.atoms.pop_back();
  }
  return d.atoms;
}

static void fail(const char* msg) {
  std::fprintf(stderr, "FAIL %s\n", msg);
  std::exit(1);
}

static bool isCh(const Atom& a, uint32_t cp, const char* rubyUtf8) {
  if (a.kind != AtomKind::Ch || a.cp != cp) {
    return false;
  }
  if (!rubyUtf8 || !*rubyUtf8) {
    return a.rubyCount == 0;
  }
  const unsigned char* p = reinterpret_cast<const unsigned char*>(rubyUtf8);
  uint8_t n = 0;
  uint32_t cps[4]{};
  while (*p && n < 4) {
    cps[n++] = utf8NextCodepoint(&p);
  }
  if (n != a.rubyCount) {
    return false;
  }
  for (uint8_t i = 0; i < n; ++i) {
    if (cps[i] != a.ruby[i]) {
      return false;
    }
  }
  return true;
}

int main() {
  {
    auto a = parse("<p><ruby><rb>吞</rb><rt>の</rt></ruby></p>");
    if (a.size() != 1 || !isCh(a[0], 0x541E, "の")) {
      fail("ruby rb");
    }
  }
  {
    auto a = parse("<p><ruby>鷲<rt>わし</rt>摑<rt>づか</rt></ruby></p>");
    if (a.size() != 2 || !isCh(a[0], 0x9DF2, "わし") || !isCh(a[1], 0x6451, "づか")) {
      fail("jukugo");
    }
  }
  {
    auto a = parse("<p>4<span class=\"tcy\">kg</span></p>");
    if (a.size() != 2 || a[0].cp != '4' || a[1].kind != AtomKind::Tcy || a[1].cp != 'k') {
      fail("tcy");
    }
  }
  {
    auto a = parse("<p><img class=\"gaiji\" src=\"x.jpeg\" alt=\"→\"/></p>");
    if (a.size() != 1 || a[0].cp != 0x2192) {
      fail("gaiji alt");
    }
  }
  {
    auto a = parse("<p>\n  アナログ\n</p>");
    if (a.size() != 4) {
      fail("pretty print");
    }
  }
  {
    auto a = parse("<p>vitti 'na crozza</p>");
    bool space = false;
    for (auto& x : a) {
      if (x.kind == AtomKind::Space) {
        space = true;
      }
    }
    if (!space) {
      fail("ascii space");
    }
  }
  {
    // 気 at SAX chunk boundary used to split E6 B0 97 and drop the kanji.
    std::string body;
    for (int i = 0; i < 60; ++i) {
      body += "あ";  // 180 UTF-8 bytes, then 気 straddles the old 184 cap
    }
    body += "気の置けない友人";
    std::string html = "<p>" + body + "</p>";
    auto a = parse(html.c_str());
    bool found = false;
    for (auto& x : a) {
      if (x.kind == AtomKind::Ch && x.cp == 0x6C17) {
        found = true;
      }
    }
    if (!found) {
      fail("気 split across text chunk");
    }
  }
  {
    auto a = parse("<p><ruby>門脇<rt>かどわき</rt></ruby></p>");
    if (a.size() != 1 || a[0].kind != AtomKind::Group || a[0].tcyCount != 2 || a[0].tcy[0] != 0x9580 ||
        a[0].tcy[1] != 0x8107 || a[0].rubyCount != 4 || a[0].ruby[0] != 0x304B || a[0].ruby[1] != 0x3069 ||
        a[0].ruby[2] != 0x308F || a[0].ruby[3] != 0x304D) {
      fail("group ruby 門脇");
    }
  }
  {
    auto a = parse("<p><em>強調</em></p>");
    if (a.size() != 2 || a[0].cp != 0x5F37 || a[1].cp != 0x8ABF || a[0].emphasis != 1 || a[1].emphasis != 1) {
      fail("em sesame");
    }
  }
  {
    auto a = parse("<p><span class=\"em-line\">線</span></p>");
    if (a.size() != 1 || a[0].emphasis != 2) {
      fail("em line");
    }
  }
  {
    auto a = parse("<p><img src=\"a.jpg\"/></p>");
    if (!a.empty()) {
      fail("img without alt became a page");
    }
  }
  {
    auto a = parse("<p><img alt=\"図\"/></p>");
    if (a.size() != 1 || a[0].kind != AtomKind::Ch || a[0].cp != 0x56F3) {
      fail("img alt");
    }
  }
  {
    auto a = parse("<p><img class=\"gaiji\" src=\"gaiji-cid13803.png\" alt=\"〓\"/></p>");
    if (a.size() != 1 || a[0].cp != 0x20B9F) {
      fail("gaiji cid");
    }
  }
  {
    auto a = parse("<h1>題</h1><p>文</p>");
    if (a.size() != 3 || a[0].cp != 0x984C || a[1].kind != AtomKind::ColumnBreak || a[2].cp != 0x6587) {
      std::fprintf(stderr, "heading atoms %zu\n", a.size());
      for (auto& x : a) {
        std::fprintf(stderr, "  kind=%u cp=%04x\n", static_cast<unsigned>(x.kind), x.cp);
      }
      fail("heading column");
    }
  }
  {
    ts::WritingMode mode = ts::WritingMode::VerticalRl;
    const char* html = "<html class=\"hltr\"><body><p>あ</p></body></html>";
    if (!ts::sniffWritingMode(html, std::strlen(html), mode) || mode != ts::WritingMode::HorizontalTb) {
      fail("sniff hltr");
    }
    mode = ts::WritingMode::HorizontalTb;
    html = "<html><body class=\"vrtl\"><p>あ</p></body></html>";
    if (!ts::sniffWritingMode(html, std::strlen(html), mode) || mode != ts::WritingMode::VerticalRl) {
      fail("sniff vrtl");
    }
    mode = ts::WritingMode::VerticalRl;
    html = "<html><head><style>.calibre{writing-mode:horizontal-tb}</style></head><body><p>あ</p></body></html>";
    if (!ts::sniffWritingMode(html, std::strlen(html), mode) || mode != ts::WritingMode::HorizontalTb) {
      fail("sniff css");
    }
  }
  std::printf("ok htmlir %s\n", "ruby/tcy/gaiji/group/em/img/heading");
  return 0;
}
