#pragma once

#include <WebServer.h>

namespace services::diag {

/** Render the /debug page. Streams in chunks; allocates ~1 KB, not a full page. */
void handleRequest(WebServer& web);

/**
 * Stream the last rendered frame as a 240x240 BMP, colored the way the panel
 * shows it (red/blue swapped when config::kDisplaySwapRedBlue is set).
 */
void handleScreenshot(WebServer& web);

}  // namespace services::diag
