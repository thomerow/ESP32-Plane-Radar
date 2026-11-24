#pragma once

#include <Print.h>

#include "hardware/lgfx_config.hpp"

/**
 * Satellite background: one 240×240 JPEG per range preset, fetched from the
 * EOX Sentinel-2 cloudless WMS in EPSG:4326 with a bbox that matches the radar's
 * flat lat/lon projection exactly, cached in LittleFS, and decoded straight into
 * the frame sprite on every redraw (the RAM has no room for a second 115 KB
 * frame).
 */
namespace services::satmap {

using PollFn = void (*)();
void setPollFn(PollFn fn);

/**
 * Download the image for this view when it is missing or was fetched for an
 * older location. Blocking HTTPS (~1–2 s); returns true when a new image was
 * stored and the display should redraw.
 */
bool refreshIfNeeded(double lat, double lon, float half_width_km);

/** True when an image for exactly this view is cached. */
bool hasImage(double lat, double lon, float half_width_km);

/**
 * Decode the cached image into sprite (full frame) and dim it to
 * brightness_pct. Returns false — sprite untouched — when nothing is cached.
 */
bool drawBackground(LGFX_Sprite& sprite, double lat, double lon,
                    float half_width_km, int brightness_pct);

void diagnostics(Print& out, double lat, double lon, float half_width_km);

}  // namespace services::satmap
