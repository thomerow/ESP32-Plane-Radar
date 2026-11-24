#pragma once

#include <cstdint>

#include <driver/gpio.h>

// Set by scripts/firmware_version.py from the git tag ("2.0.0", or
// "2.0.0-3-gabc1234[-dirty]" between releases).
#ifndef PLANE_RADAR_VERSION
#define PLANE_RADAR_VERSION "unknown"
#endif

namespace config {

/** Firmware version, shown on /info, /debug and in the serial log. */
constexpr char kFirmwareVersion[] = PLANE_RADAR_VERSION;

// --- Wi-Fi portal ---
constexpr char kPortalApName[] = "PlaneRadar-Setup";
constexpr char kPortalIp[] = "192.168.4.1";
/** mDNS host (no ".local" suffix); browser: http://plane-radar.local */
constexpr char kPortalHostname[] = "plane-radar";
constexpr char kPortalHostUrl[] = "plane-radar.local";

/** Per-attempt STA connect wait (ms); retried kWifiConnectAttempts times. */
constexpr unsigned long kWifiConnectAttemptMs = 15000;
constexpr uint8_t kWifiConnectAttempts = 3;
constexpr unsigned long kWifiPortalTimeoutSec = 0;  // 0 = no timeout while configuring
constexpr unsigned long kWifiConnectingFrameMs = 50;
/** Wait after disconnect before reconnecting (avoids portal on brief drops). */
constexpr unsigned long kWifiDownGraceMs = 4000;
/** Minimum interval between background reconnect tries. */
constexpr unsigned long kWifiReconnectIntervalMs = 15000;

// --- BOOT button (ESP32-C3 Super Mini, active LOW) ---
constexpr gpio_num_t kBootPin = GPIO_NUM_9;
constexpr unsigned long kBootResetHoldMs = 3000UL;
/** Ignore BOOT taps shorter than this (debounce). */
constexpr unsigned long kBootTapMinMs = 40UL;

// --- Display: GC9A01 1.28" round 240×240 (SPI) ---
constexpr gpio_num_t kDisplayPinRst = GPIO_NUM_0;
constexpr gpio_num_t kDisplayPinCs = GPIO_NUM_1;
constexpr gpio_num_t kDisplayPinDc = GPIO_NUM_10;
constexpr gpio_num_t kDisplayPinMosi = GPIO_NUM_3;  // display SDA
constexpr gpio_num_t kDisplayPinSclk = GPIO_NUM_4;  // display SCL

constexpr int kDisplayWidth = 240;
constexpr int kDisplayHeight = 240;

constexpr uint32_t kDisplaySpiWriteHz = 40000000;
// GC9A01 modules often need invert + BGR for correct black/green output
constexpr bool kDisplayInvert = true;
constexpr bool kDisplayRgbOrder = true;

// --- Radar center defaults (overridden via WiFi setup portal) ---
constexpr double kDefaultRadarLat = 52.3676;
constexpr double kDefaultRadarLon = 4.9041;

/** Query base; the client appends lat/<lat>/lon/<lon>/dist/<nm>. */
constexpr char kAdsbApiBase[] = "https://opendata.adsb.fi/api/v3/lat/";
/** Poll adsb.fi (API public limit: 1 req/s). */
constexpr unsigned long kAdsbFetchIntervalMs = 3000;
/**
 * ADS-B is fetched out to this multiple of the screen-edge radius. Aircraft
 * between the panel edge and this radius show up as rim dots, announcing
 * traffic before it enters the view (1.5 = half a screen width of warning).
 */
constexpr float kRimDotReachFactor = 1.5f;
/** false = hide aircraft with alt_baro "ground"; true = show them too. */
constexpr bool kAdsbShowGroundAircraft = false;

// --- Flight enrichment (origin/destination and detailed aircraft type) ---
constexpr char kFlightDataApiBase[] = "https://api.adsbdb.com/v0/";
/** One lookup at a time; successful results remain cached for six hours. */
constexpr unsigned long kFlightLookupMinIntervalMs = 750UL;
constexpr unsigned long kFlightLookupTimeoutMs = 5000UL;
constexpr unsigned long kFlightLookupFailureBackoffMs = 30000UL;
constexpr unsigned long kFlightCacheSuccessMs = 6UL * 60UL * 60UL * 1000UL;
constexpr unsigned long kFlightCacheMissMs = 10UL * 60UL * 1000UL;

// --- Weather and local time ---
constexpr char kWeatherApiBase[] = "https://api.open-meteo.com/v1/forecast";
constexpr unsigned long kWeatherFetchIntervalMs = 15UL * 60UL * 1000UL;
constexpr unsigned long kWeatherRequestTimeoutMs = 6000UL;

// --- Satellite background (EOX Sentinel-2 cloudless, CC BY-NC-SA 4.0) ---
/** WMS GetMap base; the client appends BBOX (EPSG:4326) and the image size. */
constexpr char kSatelliteWmsBase[] =
    "https://tiles.maps.eox.at/wms?SERVICE=WMS&VERSION=1.1.1&REQUEST=GetMap"
    "&LAYERS=s2cloudless-2025&STYLES=&SRS=EPSG:4326&FORMAT=image/jpeg";
constexpr unsigned long kSatelliteRequestTimeoutMs = 15000UL;
/** Wait this long after a failed download before trying again. */
constexpr unsigned long kSatelliteRetryMs = 60UL * 1000UL;

// --- Rain radar overlay (RainViewer Weather Maps API, free tier: zoom <= 7) ---
constexpr char kRainIndexUrl[] =
    "https://api.rainviewer.com/public/weather-maps.json";
/** RainViewer publishes a new frame every 10 minutes. */
constexpr unsigned long kRainCheckIntervalMs = 5UL * 60UL * 1000UL;
constexpr unsigned long kRainRetryMs = 60UL * 1000UL;
constexpr unsigned long kRainRequestTimeoutMs = 10000UL;
/** Hide the overlay once the newest frame is older than this. */
constexpr long kRainMaxAgeSec = 40L * 60L;

/**
 * Whether the panel shows red and blue swapped relative to the RGB565 words the
 * firmware sends. Every color in the firmware is defined as it should APPEAR;
 * panelColor(), the satellite image and the rain overlay pre-swap when this is
 * set. true matches the reference module (with kDisplayRgbOrder = true).
 *
 * If red aircraft show up blue on your module (or water looks orange in the
 * satellite view), set this to false. Do not flip kDisplayRgbOrder for that:
 * it was tied to this flag before, so changing it alone changed nothing.
 */
constexpr bool kDisplaySwapRedBlue = true;

/** Plain RGB565 packing (same as LovyanGFX color565). */
constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

/** RGB565 word that makes the panel show (r, g, b). */
constexpr uint16_t panelColor(uint8_t r, uint8_t g, uint8_t b) {
  return kDisplaySwapRedBlue ? rgb565(b, g, r) : rgb565(r, g, b);
}

// --- User-facing defaults ---
constexpr char kOtaUsername[] = "admin";
/** Change this in the web settings before exposing the device to other users. */
constexpr char kDefaultOtaPassword[] = "plane-radar";

// --- UI colors — status screens (as they appear on the panel) ---
constexpr uint16_t kColorBlack = panelColor(0, 0, 0);
/** Setup / connect-failed / reset screens. */
constexpr uint16_t kColorSetupBackground = panelColor(0, 255, 255);  // cyan
constexpr uint16_t kColorSetupText = panelColor(0, 0, 0);
constexpr uint16_t kTextOnBlack = panelColor(255, 255, 255);

}  // namespace config
