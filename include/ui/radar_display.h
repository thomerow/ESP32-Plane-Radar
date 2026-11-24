#pragma once

#include <Print.h>

#include <cstddef>
#include <cstdint>

namespace ui {

/** Draw the static sonar/radar grid (black disc, green overlay, labels). */
void radarDisplayDraw();

/** Redraw aircraft only (blits cached grid; no full-screen clear). */
void radarDisplayRefreshAircraft();

/**
 * Append the live projection state and the plotted aircraft as HTML, using the
 * same helpers the display draws with. Feeds the /debug page.
 */
void radarProjectionDiagnostics(Print& out);

/** Half the width of the 240 px view in km (screen edge, not the outer ring). */
float radarViewHalfWidthKm();

/**
 * The frame sprite's pixel buffer as scratch memory (115 KB). Whatever is
 * there gets overwritten; the next radarDisplay* call repaints the frame.
 */
uint8_t* radarFrameScratch(size_t* len);

/** One row (240 px) of the last frame as plain RGB565, for /screen.bmp. */
bool radarFrameRow(int y, uint16_t* out);

}  // namespace ui
