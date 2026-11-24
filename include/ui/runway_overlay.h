#pragma once

#include <LovyanGFX.hpp>
#include <Print.h>


namespace ui::runway {

/** Darkens a screen rectangle behind a label (satellite view). */
using BackdropFn = void (*)(int x, int y, int w, int h);

/**
 * backdrop != nullptr selects the satellite style: outlined runway lines and
 * labels on a semi-transparent backdrop instead of an opaque box.
 */
void drawLargeAirportRunways(lgfx::LGFXBase& gfx, BackdropFn backdrop = nullptr);

/**
 * Append the airports and runway segments currently in range as HTML,
 * using the same projection the overlay draws with. Feeds /debug.
 */
void runwayDiagnostics(Print& out);


}  // namespace ui::runway
