#include "services/diagnostics.h"

#include <Arduino.h>
#include <WiFi.h>

#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <esp_system.h>

#include "config.h"
#include "services/adsb_client.h"
#include "services/display_settings.h"
#include "services/file_cache.h"
#include "services/radar_location.h"
#include "services/rain_radar.h"
#include "services/satellite_map.h"
#include "services/weather_time.h"
#include "ui/radar_display.h"
#include "ui/radar_range.h"
#include "ui/runway_overlay.h"

/** Defined in main.cpp: reboots done to recover from a fragmented heap. */
uint32_t tlsRecoveryReboots();

namespace services::diag {
namespace {

/**
 * Print sink that flushes to the web server in chunks. The page is a few KB and
 * the largest free heap block on this board sits around 35 KB, so building the
 * whole thing as one String would be the biggest single allocation the firmware
 * makes for a web request. This keeps it near the buffer size instead.
 */
class ChunkedPage : public Print {
 public:
  explicit ChunkedPage(WebServer& web) : web_(web) { buf_.reserve(kFlushAt * 2); }

  size_t write(uint8_t c) override {
    buf_ += static_cast<char>(c);
    flushIfFull();
    return 1;
  }

  size_t write(const uint8_t* data, size_t len) override {
    buf_.concat(reinterpret_cast<const char*>(data), len);
    flushIfFull();
    return len;
  }

  void finish() {
    if (buf_.length() > 0) {
      web_.sendContent(buf_);
      buf_ = "";
    }
    web_.sendContent("");
  }

 private:
  static constexpr unsigned kFlushAt = 1024;

  void flushIfFull() {
    if (buf_.length() >= kFlushAt) {
      web_.sendContent(buf_);
      buf_ = "";
    }
  }

  WebServer& web_;
  String buf_;
};

/** SSIDs and callsigns are outside our control; never inline them raw. */
void writeEscaped(Print& out, const char* text) {
  for (const char* p = text; p != nullptr && *p != '\0'; ++p) {
    switch (*p) {
      case '<': out.print(F("&lt;")); break;
      case '>': out.print(F("&gt;")); break;
      case '&': out.print(F("&amp;")); break;
      case '"': out.print(F("&quot;")); break;
      default:
        if (static_cast<unsigned char>(*p) >= 0x20) {
          out.write(static_cast<uint8_t>(*p));
        }
        break;
    }
  }
}

void row(Print& out, const char* label, const char* value) {
  out.print(F("<tr><th>"));
  out.print(label);
  out.print(F("</th><td>"));
  writeEscaped(out, value);
  out.print(F("</td></tr>"));
}

void rowf(Print& out, const char* label, const char* fmt, ...) {
  char value[96];
  va_list args;
  va_start(args, fmt);
  vsnprintf(value, sizeof(value), fmt, args);
  va_end(args);
  row(out, label, value);
}

const char* resetReasonText() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_EXT: return "external pin";
    case ESP_RST_SW: return "software restart";
    case ESP_RST_PANIC: return "panic / exception";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "other watchdog";
    case ESP_RST_DEEPSLEEP: return "deep sleep wake";
    case ESP_RST_BROWNOUT: return "brownout (check power supply)";
    case ESP_RST_SDIO: return "SDIO";
    // The C3's USB-serial-JTAG reset does not set a reason code, so a fresh
    // flash lands here rather than on "power-on".
    default: return "unknown (normal right after a USB flash)";
  }
}

void formatUptime(char* out, size_t len, unsigned long ms) {
  const unsigned long total_s = ms / 1000UL;
  snprintf(out, len, "%lud %luh %lum %lus", total_s / 86400UL,
           (total_s % 86400UL) / 3600UL, (total_s % 3600UL) / 60UL,
           total_s % 60UL);
}

void writeHead(Print& out) {
  out.print(F(
      "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
      "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
      "<title>Plane Radar diagnostics</title><style>"
      "body{font-family:system-ui,sans-serif;background:#07101f;color:#eef6ff;"
      "margin:0}"
      "main{max-width:60rem;margin:0 auto;padding:1.5rem}"
      "h1{font-size:1.3rem;margin:0}"
      "h2{font-size:.8rem;text-transform:uppercase;letter-spacing:.08em;"
      "color:#7fb0e0;margin:1.6rem 0 .5rem}"
      "header{display:flex;flex-wrap:wrap;gap:.75rem;align-items:baseline;"
      "justify-content:space-between;border-bottom:1px solid #274464;"
      "padding-bottom:.75rem}"
      "table{width:100%;border-collapse:collapse;font-size:.86rem;"
      "background:#101d31;border:1px solid #274464;border-radius:10px;"
      "overflow:hidden}"
      "th,td{text-align:left;padding:.4rem .6rem;border-bottom:1px solid #1b2f49}"
      "tr:last-child th,tr:last-child td{border-bottom:0}"
      "th{color:#a9bad0;font-weight:600;width:13rem}"
      "td{font-variant-numeric:tabular-nums}"
      "table.grid th{width:auto;color:#7fb0e0}"
      "table.grid td{color:#eef6ff}"
      ".wrap{overflow-x:auto}"
      ".ok{color:#6ede8a}.warn{color:#ffcc66}.bad{color:#ff8a80}"
      "a{color:#73b7ff}"
      "small{color:#a9bad0}"
      "</style></head><body><main><header><h1>Plane Radar diagnostics</h1>"
      "<div><label><input type=\"checkbox\" id=\"auto\"> auto-refresh</label> "
      "&middot; <a href=\"/debug\">refresh</a> &middot; <a href=\"/\">menu</a>"
      "</div></header>"));
}

void writeSystem(Print& out) {
  out.print(F("<h2>System</h2><table>"));

  const esp_partition_t* part = esp_ota_get_running_partition();
  rowf(out, "Firmware built", "%s %s", __DATE__, __TIME__);
  // Matches the md5 of .pio/build/supermini/firmware.bin, so it answers
  // "is the build I just made actually the one running?" without guesswork.
  rowf(out, "Sketch MD5", "%s", ESP.getSketchMD5().c_str());
  rowf(out, "Running partition", "%s, %u KB of %u KB used",
       part != nullptr ? part->label : "?",
       static_cast<unsigned>(ESP.getSketchSize() / 1024),
       static_cast<unsigned>(part != nullptr ? part->size / 1024 : 0));

  char uptime[48];
  formatUptime(uptime, sizeof(uptime), millis());
  row(out, "Uptime", uptime);
  row(out, "Last reset", resetReasonText());
  rowf(out, "Chip", "%s rev %d, %d core @ %lu MHz", ESP.getChipModel(),
       ESP.getChipRevision(), ESP.getChipCores(),
       static_cast<unsigned long>(ESP.getCpuFreqMHz()));
  rowf(out, "Temperature", "%.1f °C", temperatureRead());
  out.print(F("</table>"));

  out.print(F("<h2>Memory</h2><table>"));
  const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  rowf(out, "Free heap", "%u B of %u B total",
       static_cast<unsigned>(ESP.getFreeHeap()),
       static_cast<unsigned>(ESP.getHeapSize()));
  rowf(out, "Largest free block", "%u B", static_cast<unsigned>(largest));
  rowf(out, "Lowest free ever", "%u B",
       static_cast<unsigned>(ESP.getMinFreeHeap()));
  rowf(out, "PSRAM", "%u B", static_cast<unsigned>(ESP.getPsramSize()));
  // The web server runs inside loop(), so this is the loop task's own stack.
  rowf(out, "Loop stack headroom", "%u B never used",
       static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
  out.print(F(
      "</table><p><small>A TLS handshake needs a large contiguous block. When "
      "\"largest free block\" drops well under ~40 KB, outbound HTTPS calls "
      "start failing with <code>start_ssl_client: -1</code> even though the "
      "total looks healthy. The 240&times;240 frame sprite holds 115 KB for "
      "the life of the process.</small></p>"));
}

void writeNetwork(Print& out) {
  out.print(F("<h2>Network</h2><table>"));
  const bool up = WiFi.status() == WL_CONNECTED;
  row(out, "Station", up ? "connected" : "disconnected");
  if (up) {
    row(out, "SSID", WiFi.SSID().c_str());
    rowf(out, "Signal", "%d dBm on channel %d", WiFi.RSSI(), WiFi.channel());
    rowf(out, "IP", "%s (gw %s)", WiFi.localIP().toString().c_str(),
         WiFi.gatewayIP().toString().c_str());
  }
  rowf(out, "MAC", "%s", WiFi.macAddress().c_str());
  rowf(out, "mDNS", "http://%s.local", config::kPortalHostname);
  wifi_power_t tx = WIFI_POWER_8_5dBm;
  tx = WiFi.getTxPower();
  rowf(out, "TX power", "%.1f dBm", static_cast<float>(tx) / 4.0f);
  out.print(F("</table>"));
}

void writeRadar(Print& out) {
  out.print(F("<h2>Radar and projection</h2><table>"));
  const auto& preset = ui::radar::rangeCurrent();
  rowf(out, "Center", "%.6f, %.6f", services::location::lat(),
       services::location::lon());
  rowf(out, "Range preset", "ring 3 at %.0f km, outer radius %.2f km",
       preset.ring3_km, preset.outer_km);
  rowf(out, "View radius", "%.2f km (center to panel edge)",
       ui::radar::viewRadiusKm());
  rowf(out, "ADS-B fetch radius", "%.2f km (rim dots beyond the view)",
       ui::radar::fetchRadiusKm());
  rowf(out, "Units", "%s", ui::radar::useMiles() ? "miles" : "kilometres");
  rowf(out, "Runway overlay", "%s", ui::radar::showRunways() ? "on" : "off");
  rowf(out, "Text scale", "%d %%", services::settings::textScalePercent());
  rowf(out, "Background", "%s",
       services::settings::satelliteBackground() ? "satellite" : "dark radar");
  if (services::settings::satelliteBackground()) {
    rowf(out, "Satellite brightness", "%d %%",
         services::settings::satelliteBrightnessPercent());
    services::satmap::diagnostics(out, services::location::lat(),
                                  services::location::lon(),
                                  ui::radarViewHalfWidthKm());
  }
  rowf(out, "Rain overlay", "%s",
       services::settings::rainOverlay() ? "on" : "off");
  if (services::settings::rainOverlay()) {
    services::rain::diagnostics(out);
  }
  rowf(out, "Image cache", "%u of %u KB used (LittleFS)",
       static_cast<unsigned>(services::files::usedBytes() / 1024),
       static_cast<unsigned>(services::files::totalBytes() / 1024));
  // Appends its own rows and closes the table before starting the aircraft one.
  ui::radarProjectionDiagnostics(out);
}

void writeWeather(Print& out) {
  out.print(F("<h2>Weather and time</h2><table>"));
  char line[64];
  services::weather::formatWeatherLine(line, sizeof(line));
  row(out, "Footer line", line);
  services::weather::formatDateTimeLine(line, sizeof(line));
  row(out, "Clock line", line);
  rowf(out, "Data", "%s", services::weather::valid() ? "valid" : "not fetched yet");
  rowf(out, "Footer enabled", "%s",
       services::settings::footerEnabled() ? "yes" : "no");
  out.print(F("</table>"));
}

void writeAdsb(Print& out) {
  const services::adsb::Stats& st = services::adsb::stats();
  out.print(F("<h2>ADS-B service</h2><table>"));
  rowf(out, "Fetches", "%lu ok, %lu failed",
       static_cast<unsigned long>(st.fetch_ok),
       static_cast<unsigned long>(st.fetch_failed));
  if (st.fetch_failed > 0) {
    rowf(out, "Last error code", "%d", st.last_error_code);
  }
  if (st.last_ok_ms > 0) {
    rowf(out, "Last success", "%lu s ago",
         (millis() - st.last_ok_ms) / 1000UL);
  }
  rowf(out, "TLS recovery reboots", "%lu since power-on (heap too fragmented "
       "for HTTPS)", static_cast<unsigned long>(tlsRecoveryReboots()));
  rowf(out, "Last response", "%u aircraft in fetch radius (max %u kept)",
       static_cast<unsigned>(st.last_total),
       static_cast<unsigned>(services::adsb::kMaxAircraft));
  rowf(out, "Enrichment", "%lu ok, %lu failed",
       static_cast<unsigned long>(st.enrich_ok),
       static_cast<unsigned long>(st.enrich_failed));
  rowf(out, "Enrichment cache", "%u of 48 slots used",
       static_cast<unsigned>(st.cache_used));
  out.print(F("</table>"));
}

void writeFooterNote(Print& out) {
  out.print(F(
      "<h2>About this page</h2><p><small>Read-only. Everything here is "
      "computed with the same helpers the display uses, so the screen "
      "coordinates above are the ones actually drawn. Like the rest of the "
      "portal, this page has no password &mdash; anyone on your network can "
      "read it.</small></p>"
      "<script>(function(){var b=document.getElementById('auto');if(!b)return;"
      "var t=null;try{b.checked=localStorage.getItem('prAuto')==='1'}catch(e){}"
      "function apply(){if(b.checked){t=setTimeout(function(){"
      "location.reload()},5000)}else if(t){clearTimeout(t);t=null}"
      "try{localStorage.setItem('prAuto',b.checked?'1':'0')}catch(e){}}"
      "b.addEventListener('change',apply);apply()})();</script>"
      "</main></body></html>"));
}

void putLe16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
}

void putLe32(uint8_t* p, uint32_t v) {
  putLe16(p, static_cast<uint16_t>(v));
  putLe16(p + 2, static_cast<uint16_t>(v >> 16));
}

}  // namespace

void handleScreenshot(WebServer& web) {
  constexpr int kSide = 240;
  constexpr uint32_t kHeaderSize = 14 + 40 + 12;  // file + info + RGB565 masks
  constexpr uint32_t kRowBytes = kSide * 2;
  constexpr uint32_t kFileSize = kHeaderSize + kRowBytes * kSide;

  uint16_t row[kSide];
  if (!ui::radarFrameRow(0, row)) {
    web.send(503, "text/plain", "No frame buffer (sprite not allocated)");
    return;
  }

  uint8_t header[kHeaderSize] = {};
  header[0] = 'B';
  header[1] = 'M';
  putLe32(header + 2, kFileSize);
  putLe32(header + 10, kHeaderSize);
  putLe32(header + 14, 40);
  putLe32(header + 18, kSide);
  putLe32(header + 22, static_cast<uint32_t>(-kSide));  // top-down rows
  putLe16(header + 26, 1);
  putLe16(header + 28, 16);
  putLe32(header + 30, 3);  // BI_BITFIELDS
  putLe32(header + 34, kRowBytes * kSide);
  putLe32(header + 54, 0xF800);
  putLe32(header + 58, 0x07E0);
  putLe32(header + 62, 0x001F);

  web.setContentLength(kFileSize);
  web.sendHeader("Cache-Control", "no-store");
  web.send(200, "image/bmp", "");
  web.sendContent(reinterpret_cast<const char*>(header), kHeaderSize);

  uint8_t line[kRowBytes];
  for (int y = 0; y < kSide; ++y) {
    if (!ui::radarFrameRow(y, row)) {
      memset(row, 0, sizeof(row));
    }
    for (int x = 0; x < kSide; ++x) {
      uint16_t v = row[x];
      if (config::kDisplaySwapRedBlue) {
        v = static_cast<uint16_t>((v & 0x07E0) | (v >> 11) | ((v & 0x1F) << 11));
      }
      putLe16(line + x * 2, v);
    }
    web.sendContent(reinterpret_cast<const char*>(line), kRowBytes);
  }
}

void handleRequest(WebServer& web) {
  web.setContentLength(CONTENT_LENGTH_UNKNOWN);
  web.send(200, "text/html", "");

  ChunkedPage page(web);
  writeHead(page);
  writeRadar(page);
  ui::runway::runwayDiagnostics(page);
  writeAdsb(page);
  writeWeather(page);
  writeNetwork(page);
  writeSystem(page);
  writeFooterNote(page);
  page.finish();
}

}  // namespace services::diag
