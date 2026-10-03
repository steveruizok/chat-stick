#include "util/DisplayGlyphs.h"
#include "util/OtaPolicy.h"
#include <cassert>
#include <iostream>

int main() {
  const std::string turkish = u8"ÇçĞğİıÖöŞşÜü";
  const auto cells = DisplayGlyphs::cells(turkish.data(), turkish.size());
  assert(cells.size() == 12);
  for (size_t i = 0; i < cells.size(); ++i) {
    assert(static_cast<uint8_t>(cells[i]) == 0x10 + i);
    bool hasPixels = false;
    for (auto row : DisplayGlyphs::TurkishBits[i]) hasPixels |= row != 0;
    assert(hasPixels);
  }
  assert(DisplayGlyphs::cells(cells.data(), cells.size()) == cells);
  const std::string mixed = u8"Çağrı: İstanbul, öğüş!";
  assert(DisplayGlyphs::cells(mixed.data(), mixed.size()).size() == 22);
  const std::string ui = "\x01\x02\x03\n";
  assert(DisplayGlyphs::cells(ui.data(), ui.size()) == ui);
  const std::string emoji = u8"A😀B";
  assert(DisplayGlyphs::cells(emoji.data(), emoji.size()) == "A?B");
  for (const std::string &invalid : {std::string("\xc4"), std::string("\xe0\x80\x80"),
                                  std::string("\xed\xa0\x80"), std::string("\xf4\x90\x80\x80")}) {
    assert(!DisplayGlyphs::cells(invalid.data(), invalid.size()).empty());
    assert(DisplayGlyphs::cells(invalid.data(), invalid.size()).find_first_not_of('?') == std::string::npos);
  }
  const std::string route = "/firmware/download?device=m5-stick";
  const auto allowed = [&](const std::string &url, bool ca = true) {
    return OtaPolicy::allows(url, "https://firmware.example", "firmware.example", ca, "m5-stick");
  };
  assert(allowed("https://firmware.example" + route));
  assert(!allowed("https://firmware.example" + route, false));
  for (const std::string origin : {"http://firmware.example", "https://evil.example",
                                  "https://firmware.example.evil", "https://firmware.example:444",
                                  "https://firmware.example@evil.example"}) {
    assert(!allowed(origin + route));
  }
  assert(!allowed("https://firmware.example/firmware/download?device=waveshare"));
  assert(!allowed("https://firmware.example" + route + "&redirect=evil"));
  assert(OtaPolicy::allows("http://192.168.1.10:8787" + route,
                          "http://192.168.1.10:8787", "192.168.1.10", false, "m5-stick"));
  assert(!OtaPolicy::allows("http://203.0.113.1" + route,
                           "http://203.0.113.1", "203.0.113.1", false, "m5-stick"));
  assert(!OtaPolicy::isLanHost("192.168.1.10.evil"));
  assert(!OtaPolicy::isLanHost("172.32.0.1"));
  assert(OtaPolicy::isLanHost("172.16.0.1"));
  std::cout << "Firmware UTF-8 and OTA policy tests passed\n";
}
