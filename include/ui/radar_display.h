#pragma once

#include <Print.h>

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

}  // namespace ui
