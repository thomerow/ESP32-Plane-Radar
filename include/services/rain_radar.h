#pragma once

#include <cstddef>
#include <cstdint>

#include <Print.h>

#include "hardware/lgfx_config.hpp"

/**
 * Rain radar overlay from the RainViewer Weather Maps API (free tier, zoom 7).
 *
 * The newest radar frame is fetched as one 256×256 RGBA PNG centered on the
 * radar location and cached in LittleFS. Decoding it needs a 32 KB inflate
 * window that the heap cannot spare, so the decoder borrows the frame sprite's
 * buffer as scratch (the next redraw repaints it anyway) and keeps only a small
 * intensity grid that is resampled onto every frame.
 */
namespace services::rain {

using PollFn = void (*)();
void setPollFn(PollFn fn);

/** Returns a scratch buffer and its size, or nullptr. Contents get clobbered. */
using ScratchFn = uint8_t* (*)(size_t* len);
void setScratchProvider(ScratchFn fn);

/**
 * Check for a newer frame when due, download it, and rebuild the intensity
 * grid for the current view. Returns true when the overlay changed and the
 * display should redraw (the scratch buffer was clobbered in that case).
 */
bool refreshIfDue(double lat, double lon, float half_width_km);

/** Blend the overlay into the frame; no-op unless a fresh grid fits this view. */
void drawOverlay(LGFX_Sprite& sprite, double lat, double lon,
                 float half_width_km);

void diagnostics(Print& out);

}  // namespace services::rain
