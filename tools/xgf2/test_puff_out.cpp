// c++ -std=c++20 -I lib/Typesetter tools/xgf2/test_puff_out.cpp lib/Typesetter/puff.c -o /tmp/test_puff_out && /tmp/test_puff_out
#include "puff.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <zlib.h>

struct MemIn {
  const uint8_t* p;
  unsigned long n;
};

static int memRefill(unsigned char* buf, unsigned long cap, void* user) {
  auto* in = static_cast<MemIn*>(user);
  if (in->n == 0) {
    return 0;
  }
  const unsigned long k = cap < in->n ? cap : in->n;
  memcpy(buf, in->p, k);
  in->p += k;
  in->n -= k;
  return static_cast<int>(k);
}

struct MemOut {
  std::vector<uint8_t> d;
};

static int memFlush(const unsigned char* buf, unsigned long n, void* user) {
  auto* o = static_cast<MemOut*>(user);
  o->d.insert(o->d.end(), buf, buf + n);
  return 0;
}

int main() {
  const char* path = "/tmp/izu-epub/OEBPS/Text/E0083610000000000000_0009.xhtml";
  FILE* f = std::fopen(path, "rb");
  if (!f) {
    std::fprintf(stderr, "missing %s (unzip the epub first)\n", path);
    return 2;
  }
  std::fseek(f, 0, SEEK_END);
  const long n = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  std::vector<uint8_t> raw(static_cast<size_t>(n));
  if (std::fread(raw.data(), 1, raw.size(), f) != raw.size()) {
    return 2;
  }
  std::fclose(f);

  z_stream zs{};
  deflateInit2(&zs, 9, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY);
  std::vector<uint8_t> comp(raw.size() + 64);
  zs.next_in = raw.data();
  zs.avail_in = static_cast<uInt>(raw.size());
  zs.next_out = comp.data();
  zs.avail_out = static_cast<uInt>(comp.size());
  if (deflate(&zs, Z_FINISH) != Z_STREAM_END) {
    std::fprintf(stderr, "deflate failed\n");
    return 1;
  }
  const unsigned long clen = zs.total_out;
  deflateEnd(&zs);

  unsigned char window[32768];
  unsigned char inbuf[512];
  MemIn in{comp.data(), clen};
  MemOut out;
  unsigned long produced = 0;
  const int st = puff_stream_out(memRefill, &in, inbuf, sizeof(inbuf), window, sizeof(window), memFlush, &out,
                                 &produced);
  if (st != 0) {
    std::fprintf(stderr, "puff-out st=%d\n", st);
    return 1;
  }
  if (produced != raw.size() || out.d != raw) {
    std::fprintf(stderr, "mismatch produced=%lu expect=%zu out=%zu\n", produced, raw.size(), out.d.size());
    return 1;
  }
  std::printf("ok puff-out %lu bytes\n", produced);
  return 0;
}
