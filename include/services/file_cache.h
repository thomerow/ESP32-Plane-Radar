#pragma once

#include <cstddef>

#include <LittleFS.h>

/**
 * LittleFS on the "spiffs" data partition, holding downloaded map images. Both
 * the satellite background and the rain overlay stream their HTTPS responses
 * straight into files here so no response ever has to fit in RAM.
 */
namespace services::files {

using PollFn = void (*)();

/** Mount once (formats the partition on first use). Safe to call repeatedly. */
bool mount();
bool mounted();

/**
 * GET url over HTTPS and write the body to path (via a temp file, so a failed
 * download never replaces a good one). Returns the HTTP status or a negative
 * HTTPClient error; *bytes receives the stored size on success.
 */
int download(const char* url, const char* path, unsigned long timeout_ms,
             PollFn poll, size_t* bytes);

/** Total / used bytes of the partition, for the diagnostics page. */
size_t totalBytes();
size_t usedBytes();

}  // namespace services::files
