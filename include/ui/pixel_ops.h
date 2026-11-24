#pragma once

#include <algorithm>
#include <cstdint>

#include "hardware/lgfx_config.hpp"

/**
 * Direct pixel work on a 16-bit LGFX_Sprite buffer. LovyanGFX keeps sprite
 * pixels as byte-swapped RGB565, so every helper unswaps, works on the
 * channels, and swaps back.
 */
namespace ui::pixels {

inline uint16_t unswap(uint16_t v) { return static_cast<uint16_t>((v >> 8) | (v << 8)); }

/** Pack 8-bit channels into the sprite's stored (byte-swapped) RGB565 format. */
inline uint16_t packStored(uint8_t r, uint8_t g, uint8_t b) {
  const uint16_t v = static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
  return unswap(v);
}

/** Scale one stored pixel's brightness by scale/256. */
inline uint16_t scaleStored(uint16_t stored, uint16_t scale) {
  const uint16_t v = unswap(stored);
  const uint16_t r = static_cast<uint16_t>((((v >> 11) & 0x1F) * scale) >> 8);
  const uint16_t g = static_cast<uint16_t>((((v >> 5) & 0x3F) * scale) >> 8);
  const uint16_t b = static_cast<uint16_t>(((v & 0x1F) * scale) >> 8);
  return unswap(static_cast<uint16_t>((r << 11) | (g << 5) | b));
}

/** Darken a clipped rectangle to scale/256 of its brightness (semi-transparent backdrop). */
inline void darkenRect(LGFX_Sprite& sprite, int x, int y, int w, int h, uint16_t scale) {
  auto* buf = static_cast<uint16_t*>(sprite.getBuffer());
  if (buf == nullptr || sprite.getColorDepth() != 16) {
    return;
  }
  const int sw = sprite.width();
  const int sh = sprite.height();
  const int x0 = std::max(0, x);
  const int y0 = std::max(0, y);
  const int x1 = std::min(sw, x + w);
  const int y1 = std::min(sh, y + h);
  for (int yy = y0; yy < y1; ++yy) {
    uint16_t* row = buf + yy * sw;
    for (int xx = x0; xx < x1; ++xx) {
      row[xx] = scaleStored(row[xx], scale);
    }
  }
}

}  // namespace ui::pixels
