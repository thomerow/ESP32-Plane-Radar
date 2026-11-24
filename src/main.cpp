/**
 * Plane Radar — WiFi setup, then radar UI on the round GC9A01 display.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <esp_attr.h>
#include <esp_heap_caps.h>

#include "config.h"
#include "hardware/display.h"
#include "services/adsb_client.h"
#include "services/display_settings.h"
#include "services/file_cache.h"
#include "services/ota_update.h"
#include "services/radar_location.h"
#include "services/rain_radar.h"
#include "services/satellite_map.h"
#include "services/weather_time.h"
#include "services/wifi_setup.h"
#include "ui/radar_display.h"
#include "ui/radar_range.h"
#include "ui/status_screens.h"

namespace {

bool g_radar_visible = false;
unsigned long g_wifi_down_since = 0;
unsigned long g_last_reconnect_ms = 0;
unsigned long g_last_adsb_fetch_ms = 0;

/**
 * Heap reserved at the very start of setup() and freed only once Wi-Fi, mDNS,
 * the LAN web portal and the 115 KB frame sprite have all taken their memory.
 * Without it, on about one boot in four (also before the satellite/rain work)
 * those allocations split the heap so that no block was large enough for
 * mbedTLS, and every HTTPS call failed until the next reboot.
 */
constexpr size_t kTlsReserveBytes = 40 * 1024;
void* g_tls_reserve = nullptr;

/**
 * Last resort for the fragmented-heap boot: mbedTLS needs two contiguous
 * ~16.7 KB buffers per connection and on some boots never finds them again.
 * A reboot almost always lands in a usable layout. The attempt counter lives
 * in RTC memory (survives the restart, not a power cycle) so a persistent
 * cause cannot turn into a reboot loop.
 */
constexpr uint32_t kTlsRecoveryAfterFailures = 3;  // ~30 s of failed fetches
constexpr uint32_t kTlsRecoveryMaxReboots = 5;
constexpr uint32_t kRecoveryMagic = 0x504C5241;  // "PLRA"
RTC_NOINIT_ATTR uint32_t g_recovery_magic;
RTC_NOINIT_ATTR uint32_t g_recovery_reboots;

void initRecoveryCounter() {
  if (g_recovery_magic != kRecoveryMagic) {
    g_recovery_magic = kRecoveryMagic;
    g_recovery_reboots = 0;
  }
}

void checkTlsRecovery() {
  const auto& st = services::adsb::stats();
  if (st.tls_alloc_fail_streak == 0 && st.fetch_ok > 0) {
    g_recovery_reboots = 0;  // healthy again
    return;
  }
  if (st.tls_alloc_fail_streak < kTlsRecoveryAfterFailures ||
      g_recovery_reboots >= kTlsRecoveryMaxReboots) {
    return;
  }
  ++g_recovery_reboots;
  Serial.printf("heap: TLS cannot allocate its buffers, rebooting (%u/%u)\n",
                static_cast<unsigned>(g_recovery_reboots),
                static_cast<unsigned>(kTlsRecoveryMaxReboots));
  delay(100);
  ESP.restart();
}

void releaseTlsReserve() {
  if (g_tls_reserve == nullptr) {
    return;
  }
  free(g_tls_reserve);
  g_tls_reserve = nullptr;
  Serial.printf("heap: TLS reserve released, largest block %u B\n",
                static_cast<unsigned>(
                    heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
}

void showRadarIfConnected() {
  if (WiFi.status() != WL_CONNECTED) {
    g_radar_visible = false;
    return;
  }
  services::weather::begin();
  ui::radarDisplayDraw();
  g_radar_visible = true;
}

void onRangeTap() {
  ui::radar::rangeNext();
  char range_label[12];
  ui::radar::formatCurrentRing3Label(range_label, sizeof(range_label));
  Serial.printf("Range: %s (outer ~%.0f km)\n", range_label,
                ui::radar::rangeCurrent().outer_km);

  if (g_radar_visible && WiFi.status() == WL_CONNECTED) {
    ui::radarDisplayDraw();
  }
}

void handleBootButton() {
  bootButtonPollLongPress();
  if (bootButtonConsumeTap()) {
    onRangeTap();
  }
}

/** Satellite image for the current view; downloads once per range/location. */
bool refreshSatelliteIfNeeded() {
  if (!services::settings::satelliteBackground()) {
    return false;
  }
  return services::satmap::refreshIfNeeded(services::location::lat(),
                                           services::location::lon(),
                                           ui::radarViewHalfWidthKm());
}

bool refreshRainIfDue() {
  if (!services::settings::rainOverlay()) {
    return false;
  }
  return services::rain::refreshIfDue(services::location::lat(),
                                      services::location::lon(),
                                      ui::radarViewHalfWidthKm());
}

void fetchAndDrawAircraft() {
  const float fetch_km = ui::radar::fetchRadiusKm();
  if (!services::adsb::fetchUpdate(services::location::lat(),
                                   services::location::lon(), fetch_km)) {
    checkTlsRecovery();
    handleBootButton();
    return;
  }
  checkTlsRecovery();
  ui::radarDisplayRefreshAircraft();
  handleBootButton();
}

}  // namespace

uint32_t tlsRecoveryReboots() { return g_recovery_reboots; }

namespace {

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("Plane Radar");
  g_tls_reserve = malloc(kTlsReserveBytes);  // see releaseTlsReserve()
  initRecoveryCounter();

  bootButtonInit();
  displayInit();
  if (wifiShowsSetupScreenOnBoot()) {
    statusScreenPortal();
  }
  services::location::init();
  ui::radar::rangeInit();
  services::settings::init();
  services::adsb::setPollFn(wifiLoop);
  services::weather::setPollFn(wifiLoop);
  services::satmap::setPollFn(wifiLoop);
  services::rain::setPollFn(wifiLoop);
  services::rain::setScratchProvider(ui::radarFrameScratch);
  services::files::mount();

  if (wifiSetupConnect()) {
    showRadarIfConnected();
  }
}

void loop() {
  handleBootButton();
  wifiLoop();
  if (g_radar_visible) {
    // Wi-Fi, web portal and frame sprite are in place: hand the hole to TLS.
    releaseTlsReserve();
  }

  if (services::ota::inProgress()) {
    delay(10);
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    if (g_radar_visible) {
      Serial.println("WiFi lost — will reconnect");
      g_radar_visible = false;
    }

    if (g_wifi_down_since == 0) {
      g_wifi_down_since = millis();
    }

    const unsigned long down_ms = millis() - g_wifi_down_since;
    if (down_ms >= config::kWifiDownGraceMs &&
        millis() - g_last_reconnect_ms >= config::kWifiReconnectIntervalMs) {
      g_last_reconnect_ms = millis();
      if (wifiReconnect()) {
        g_wifi_down_since = 0;
        showRadarIfConnected();
      }
    }
  } else {
    g_wifi_down_since = 0;
    if (!g_radar_visible) {
      showRadarIfConnected();
    } else if (millis() - g_last_adsb_fetch_ms >= config::kAdsbFetchIntervalMs) {
      g_last_adsb_fetch_ms = millis();
      fetchAndDrawAircraft();
    } else if (services::weather::refreshIfDue(
                   services::location::lat(), services::location::lon())) {
      ui::radarDisplayRefreshAircraft();
    } else if (refreshSatelliteIfNeeded()) {
      ui::radarDisplayRefreshAircraft();
    } else if (refreshRainIfDue()) {
      ui::radarDisplayRefreshAircraft();
    } else if (services::adsb::enrichOnePending()) {
      ui::radarDisplayRefreshAircraft();
    }
  }

  delay(10);
}
