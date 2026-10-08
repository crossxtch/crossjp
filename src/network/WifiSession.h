#pragma once

class Gfx;

// Lifetime of a Wi-Fi file-transfer visit. The ESP32-C3 has one DRAM heap:
// ScratchHeap's reservation and Wi-Fi/mDNS cannot both be allocated after
// the framebuffer has split the heap, so this session frees that reservation
// and restores it by restarting when the visit ends.
namespace WifiSession {

// Free the reader scratch reservation. Call once when entering the Wi-Fi flow.
void begin();

// Disconnect STA and reboot. setup() reserves scratch from a clean heap
// and lands on Home. Does not return.
[[noreturn]] void end(Gfx& gfx);

}  // namespace WifiSession
