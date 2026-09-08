#pragma once

#include <LovyanGFX.hpp>
#include <Print.h>


namespace ui::runway {

void drawLargeAirportRunways(lgfx::LGFXBase& gfx);

/**
 * Append the airports and runway segments currently in range as HTML,
 * using the same projection the overlay draws with. Feeds /debug.
 */
void runwayDiagnostics(Print& out);


}  // namespace ui::runway
