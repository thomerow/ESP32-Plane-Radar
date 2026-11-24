#pragma once

#include <cstddef>
#include <cstdint>

namespace services::adsb {

/** Counters behind the diagnostics page; cheap enough to keep always on. */
struct Stats {
  uint32_t fetch_ok = 0;
  uint32_t fetch_failed = 0;
  /** Last non-OK HTTP/HTTPClient code, 0 while nothing has failed yet. */
  int last_error_code = 0;
  unsigned long last_ok_ms = 0;
  uint32_t enrich_ok = 0;
  uint32_t enrich_failed = 0;
  /** Enrichment cache entries currently holding data. */
  size_t cache_used = 0;
  /** Aircraft in the last response, before the nearest-64 cut. */
  size_t last_total = 0;
  /**
   * Consecutive fetches that failed because mbedTLS could not allocate its
   * buffers (heap too fragmented); reset by any fetch that gets past TLS.
   */
  uint32_t tls_alloc_fail_streak = 0;
};

const Stats& stats();

struct Aircraft {
  float lat;
  float lon;
  float nose_deg;
  float track_deg;
  float gs_knots;
  char hex[7];
  char callsign[9];
  /** IATA/ICAO origin-destination pair, for example "BOS-IND". */
  char route[10];
  /** Compact detailed model, for example "B737-800". */
  char type[18];
  char alt[12];
};

constexpr size_t kMaxAircraft = 64;

size_t aircraftCount();
const Aircraft* aircraftList();

/** Hook invoked during long HTTP I/O (e.g. wifiLoop). Optional. */
using PollFn = void (*)();
void setPollFn(PollFn fn);

/** Fetch aircraft within fetch_radius_km of center_lat/lon from adsb.fi. */
bool fetchUpdate(double center_lat, double center_lon, float fetch_radius_km);

/**
 * Look up one uncached aircraft through ADSBDB. Results are rate-limited and
 * cached. Returns true when a visible route or type changed.
 */
bool enrichOnePending();

}  // namespace services::adsb
