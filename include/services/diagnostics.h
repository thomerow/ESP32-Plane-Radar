#pragma once

#include <WebServer.h>

namespace services::diag {

/** Render the /debug page. Streams in chunks; allocates ~1 KB, not a full page. */
void handleRequest(WebServer& web);

}  // namespace services::diag
