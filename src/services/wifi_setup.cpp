#include "services/wifi_setup.h"

#include <WiFi.h>
#include <WiFiManager.h>

#include <cstdio>

#include <Preferences.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <esp_wifi.h>

#ifdef WM_MDNS
#include <ESPmDNS.h>
#endif

#include "config.h"
#include "services/display_settings.h"
#include "services/ota_update.h"
#include "services/radar_location.h"
#include "services/diagnostics.h"
#include "ui/radar_range.h"
#include "ui/status_screens.h"

portMUX_TYPE s_boot_mux = portMUX_INITIALIZER_UNLOCKED;
volatile bool s_boot_tap_pending = false;
volatile bool s_boot_is_down = false;
volatile unsigned long s_boot_down_ms = 0;
bool s_long_press_handled = false;
bool s_boot_interrupt_attached = false;

void IRAM_ATTR onBootButtonIsr() {
  const bool down = digitalRead(config::kBootPin) == LOW;
  const unsigned long now = millis();
  portENTER_CRITICAL_ISR(&s_boot_mux);
  if (down) {
    s_boot_is_down = true;
    s_boot_down_ms = now;
  } else if (s_boot_is_down) {
    const unsigned long held = now - s_boot_down_ms;
    if (held >= config::kBootTapMinMs && held < config::kBootResetHoldMs) {
      s_boot_tap_pending = true;
    }
    s_boot_is_down = false;
  }
  portEXIT_CRITICAL_ISR(&s_boot_mux);
}

void initBootButton() {
  pinMode(config::kBootPin, INPUT_PULLUP);
  if (s_boot_interrupt_attached) {
    return;
  }
  attachInterrupt(digitalPinToInterrupt(static_cast<uint8_t>(config::kBootPin)),
                  onBootButtonIsr, CHANGE);
  s_boot_interrupt_attached = true;
}

namespace {

/** Separate from planeradar prefs (rangeInit) to avoid NVS handle conflicts. */
constexpr char kWifiPrefsNamespace[] = "wifi";
constexpr char kPrefsForcePortalKey[] = "portal";

bool s_force_config_portal = false;
/**
 * WiFiManager with an /info page that matches this firmware. The stock page
 * lists the library's own routes (including "/u OTA Update", which this
 * firmware took over for authenticated uploads) and none of ours, adds the
 * other OTA slot to the sketch "total", converts the chip temperature to
 * Fahrenheit wrongly, and takes its build date from a library file that is not
 * always recompiled. Everything else is rendered with the library's own rows.
 */
class PlaneRadarWiFiManager : public WiFiManager {
 public:
  void handleInfoPage() {
    handleRequest();
    String page = getHTTPHead(FPSTR(S_titleinfo));
    reportStatus(page);

    for (const char* id : {"esphead", "uptime", "chipid", "chiprev", "idesize",
                           "flashsize", "cpufreq", "freeheap"}) {
      page += getInfoData(id);
    }
    const esp_partition_t* part = esp_ota_get_running_partition();
    const unsigned used = static_cast<unsigned>(ESP.getSketchSize());
    const unsigned slot = part != nullptr ? static_cast<unsigned>(part->size) : 0;
    page += F("<dt>Memory - Sketch size</dt><dd>Used / app partition bytes<br/>");
    page += String(used) + " / " + String(slot);
    page += "<br/><progress value='" + String(used) + "' max='" + String(slot) +
            "'></progress></dd>";
    page += getInfoData("lastreset");
    page += F("<dt>Temperature</dt><dd>");
    page += String(temperatureRead(), 1);
    page += F(" &deg;C (uncalibrated chip sensor)</dd>");

    for (const char* id : {"wifihead", "conx", "stassid", "staip", "stagw",
                           "stasub", "dnss", "host"}) {
      page += getInfoData(id);
    }
    page += F("<dt>mDNS name</dt><dd>");
    page += config::kPortalHostUrl;
    page += F("</dd>");
    for (const char* id : {"stamac", "apssid", "wifiaphead", "apip", "apmac",
                           "aphost", "apbssid"}) {
      page += getInfoData(id);
    }
    page += F("</dl>");

    page += F("<h3>About</h3><hr><dl>");
    page += getInfoData("aboutver");
    page += getInfoData("aboutarduinover");
    page += getInfoData("aboutidfver");
    page += F("<dt>Firmware</dt><dd>Plane Radar, sketch MD5 ");
    page += ESP.getSketchMD5();
    page += F("<br/><small>Equals the md5sum of firmware.bin</small></dd></dl>");

    if (_showInfoErase) {
      page += FPSTR(HTTP_ERASEBTN);
    }
    page += F(
        "<br/><h3>Available pages</h3><hr>"
        "<table class='table'><thead><tr><th>Page</th><th>Function</th></tr>"
        "</thead><tbody>"
        "<tr><td><a href='/'>/</a></td><td>Menu</td></tr>"
        "<tr><td><a href='/wifi'>/wifi</a></td><td>Scan for networks and "
        "change the Wi-Fi connection (<a href='/0wifi'>/0wifi</a> without "
        "scanning)</td></tr>"
        "<tr><td><a href='/param'>/param</a></td><td>Setup: radar location "
        "(map picker), units, runways, footer and weather, text size, "
        "satellite background and brightness, rain radar, OTA password</td></tr>"
        "<tr><td><a href='/info'>/info</a></td><td>This page</td></tr>"
        "<tr><td><a href='/debug'>/debug</a></td><td>Diagnostics: projection, "
        "aircraft, airports, satellite and rain status, ADS-B and TLS "
        "counters, memory</td></tr>"
        "<tr><td><a href='/screen.bmp'>/screen.bmp</a></td><td>The current "
        "display as a 240&times;240 image</td></tr>"
        "<tr><td><a href='/firmware'>/firmware</a></td><td>Firmware update "
        "(password protected, user <code>admin</code>; <code>/update</code> "
        "leads here too)</td></tr>"
        "<tr><td>/restart</td><td>Reboot the device</td></tr>"
        "<tr><td>/exit</td><td>Close the configuration portal until the next "
        "reboot</td></tr>"
        "<tr><td>/close</td><td>Close the captive-portal popup (setup access "
        "point only)</td></tr>"
        "<tr><td>/erase</td><td>Erase the Wi-Fi configuration and reboot; the "
        "device opens the setup access point again</td></tr>"
        "</tbody></table>");
    page += FPSTR(HTTP_END);
    HTTPSend(page);
  }
};

PlaneRadarWiFiManager s_wm;
bool s_wm_configured = false;

void ensureWifiManager();
void startLanWebPortal();
void stopLanWebPortal();
bool wifiLinkUp();
void attachSettingsRoutes();

constexpr int kCoordParamLen = 20;
constexpr char kLatitudeInputAttrs[] =
    "type=\"number\" step=\"0.000001\" min=\"-90\" max=\"90\"";
constexpr char kLongitudeInputAttrs[] =
    "type=\"number\" step=\"0.000001\" min=\"-180\" max=\"180\"";
constexpr int kOtaPasswordParamLen =
    static_cast<int>(services::settings::kOtaPasswordMaxLen);
constexpr int kTextScaleParamLen = 4;

WiFiManagerParameter s_param_lat("radar_lat", "Latitude (deg)", "0",
                                kCoordParamLen, kLatitudeInputAttrs);
WiFiManagerParameter s_param_lon("radar_lon", "Longitude (deg)", "0",
                                kCoordParamLen, kLongitudeInputAttrs);

// Map picker for the two coordinate fields above. Leaflet and the OpenStreetMap
// tiles are fetched by the browser from a CDN, so the map only appears when the
// browser has internet — i.e. on the LAN portal, not on the setup AP. When the
// load fails the block hides itself and the numeric inputs remain the only way
// in. The picker never posts anything of its own; it just writes radar_lat and
// radar_lon, which the existing /paramsave handler already stores.
//
// Two constraints from the surrounding page: the markup lands inside
// WiFiManager's settings <form>, so buttons must be type="button" and Enter has
// to be swallowed; and the portal CSS styles every <div>, which would shift
// Leaflet's absolutely positioned panes, so #map_pick resets that.
constexpr char kMapPickerHtml[] = R"HTML(
<style>
#map_pick{height:260px;padding:0;margin:0;border:1px solid #ccc;border-radius:.3rem;background:#e8e6e1}
#map_pick div{padding:0;margin:0}
#map_pick .leaflet-control{margin:10px}
#map_pick .leaflet-control-attribution{margin:0;padding:0 4px;font-size:10px}
#map_row{display:flex;gap:5px;padding:0;margin:0 0 5px}
#map_q{flex:1;margin:0}
#map_go{width:auto;flex:0 0 auto;margin:0;padding:0 14px;font-size:1em;line-height:2rem}
#map_msg{padding:0;margin:3px 0 5px;font-size:.85em;color:#555}
</style>
<div id="map_ui" style="display:none;padding:0;margin:0">
<div id="map_row"><input id="map_q" type="text" placeholder="Search a place">
<button type="button" id="map_go">Find</button></div>
<div id="map_pick"></div></div>
<div id="map_msg">Loading map...</div>
<script>(function(){
var CDN='https://cdnjs.cloudflare.com/ajax/libs/leaflet/1.9.4/';
var la=document.getElementById('radar_lat'),lo=document.getElementById('radar_lon');
if(!la||!lo)return;
var ui=document.getElementById('map_ui'),msg=document.getElementById('map_msg');
var map=null,mark=null,settled=false;
function say(t){msg.textContent=t}
function num(el){var v=parseFloat(el.value);return isFinite(v)?v:0}
function fail(){if(settled)return;settled=true;
say('Map needs internet access - enter the coordinates manually.')}
function write(ll){ll=ll.wrap();la.value=ll.lat.toFixed(6);lo.value=ll.lng.toFixed(6);
say('Selected '+la.value+', '+lo.value+' - remember to save.')}
function place(ll,z){ll=ll.wrap();mark.setLatLng(ll);map.setView(ll,z||map.getZoom());write(ll)}
function search(){var q=document.getElementById('map_q').value.replace(/^\s+|\s+$/g,'');
if(!q)return;say('Searching...');
fetch('https://nominatim.openstreetmap.org/search?format=jsonv2&limit=1&q='+encodeURIComponent(q))
.then(function(r){return r.json()})
.then(function(j){if(!j||!j.length){say('No match for that place.');return}
place(L.latLng(parseFloat(j[0].lat),parseFloat(j[0].lon)),12)})
.catch(function(){say('Search failed - try again.')})} 
function init(){if(settled)return;settled=true;
ui.style.display='block';
var c=L.latLng(num(la),num(lo));
map=L.map('map_pick').setView(c,11);
L.tileLayer('https://tile.openstreetmap.org/{z}/{x}/{y}.png',
{maxZoom:19,attribution:'&copy; <a href="https://www.openstreetmap.org/copyright">OpenStreetMap</a>'}).addTo(map);
mark=L.marker(c,{draggable:true}).addTo(map);
window.addEventListener('load',function(){map.invalidateSize()});
map.on('click',function(e){mark.setLatLng(e.latlng);write(e.latlng)});
mark.on('dragend',function(){write(mark.getLatLng())});
function sync(){var a=parseFloat(la.value),b=parseFloat(lo.value);
if(isFinite(a)&&isFinite(b))place(L.latLng(a,b))}
la.addEventListener('change',sync);lo.addEventListener('change',sync);
document.getElementById('map_go').addEventListener('click',search);
document.getElementById('map_q').addEventListener('keydown',function(e){
if(e.keyCode===13){e.preventDefault();search()}});
say('Click the map or drag the marker to set the radar center.')}
// Wait for the parser before measuring: the portal's .wrap is an inline-block,
// so its shrink-to-fit width is only final once the rest of the form exists.
// With Leaflet served from cache, onload fires mid-parse and the map would size
// itself to a container that is still growing, leaving tiles short of the edge.
function start(){if(document.readyState==='loading'){
document.addEventListener('DOMContentLoaded',init)}else{init()}}
// The stylesheet goes in first: the browser holds the script below until a
// preceding pending stylesheet has loaded, so Leaflet always starts styled.
var css=document.createElement('link');css.rel='stylesheet';css.href=CDN+'leaflet.css';
document.head.appendChild(css);
var js=document.createElement('script');js.src=CDN+'leaflet.js';
js.onload=start;js.onerror=fail;document.head.appendChild(js);
setTimeout(fail,8000);
})();</script>
)HTML";
WiFiManagerParameter s_param_map_picker(kMapPickerHtml);

char s_miles_checkbox_attrs[32] = "type=\"checkbox\"";
WiFiManagerParameter s_param_miles("use_miles", "Display distances in miles", "T", 2,
                                   s_miles_checkbox_attrs, WFM_LABEL_AFTER);

char s_runways_checkbox_attrs[32] = "type=\"checkbox\"";
WiFiManagerParameter s_param_runways("show_runways", "Show airport runways", "T", 2,
                                     s_runways_checkbox_attrs, WFM_LABEL_AFTER);

char s_footer_checkbox_attrs[32] = "type=\"checkbox\"";
WiFiManagerParameter s_param_footer("show_footer", "Show weather and clock", "T",
                                    2, s_footer_checkbox_attrs,
                                    WFM_LABEL_AFTER);

char s_weather_checkbox_attrs[32] = "type=\"checkbox\"";
WiFiManagerParameter s_param_weather(
    "show_weather", "Show current weather", "T", 2,
    s_weather_checkbox_attrs, WFM_LABEL_AFTER);

char s_fahrenheit_checkbox_attrs[32] = "type=\"checkbox\"";
WiFiManagerParameter s_param_fahrenheit(
    "temp_f", "Temperature in Fahrenheit", "T", 2,
    s_fahrenheit_checkbox_attrs, WFM_LABEL_AFTER);

char s_clock24_checkbox_attrs[32] = "type=\"checkbox\"";
WiFiManagerParameter s_param_clock24("clock_24", "Use 24-hour clock", "T", 2,
                                     s_clock24_checkbox_attrs,
                                     WFM_LABEL_AFTER);

WiFiManagerParameter s_param_after_clock_break("<br/>");

constexpr char kTextScaleAttrs[] =
    "type=\"range\" min=\"80\" max=\"130\" step=\"5\" "
    "oninput=\"document.getElementById('text_scale_value').value="
    "this.value+'%'\"";
WiFiManagerParameter s_param_text_scale(
    "text_scale", "Radar text size", "110", kTextScaleParamLen,
    kTextScaleAttrs);
WiFiManagerParameter s_param_text_scale_output(
    "<div style=\"text-align:center;margin-top:-5px\">"
    "<output id=\"text_scale_value\" for=\"text_scale\"></output></div>"
    "<script>(function(){var s=document.getElementById('text_scale'),"
    "o=document.getElementById('text_scale_value');"
    "if(s&&o)o.value=s.value+'%';})();</script>");

char s_satellite_checkbox_attrs[32] = "type=\"checkbox\"";
WiFiManagerParameter s_param_satellite(
    "sat_bg", "Satellite background (hides the grid)", "T", 2,
    s_satellite_checkbox_attrs, WFM_LABEL_AFTER);

WiFiManagerParameter s_param_after_satellite_break("<br/>");

constexpr int kSatBrightnessParamLen = 4;
constexpr char kSatBrightnessAttrs[] =
    "type=\"range\" min=\"20\" max=\"100\" step=\"5\" "
    "oninput=\"document.getElementById('sat_pct_value').value="
    "this.value+'%'\"";
WiFiManagerParameter s_param_sat_brightness(
    "sat_pct", "Satellite image brightness", "100", kSatBrightnessParamLen,
    kSatBrightnessAttrs);
WiFiManagerParameter s_param_sat_brightness_output(
    "<div style=\"text-align:center;margin-top:-5px\">"
    "<output id=\"sat_pct_value\" for=\"sat_pct\"></output></div>"
    "<script>(function(){var s=document.getElementById('sat_pct'),"
    "o=document.getElementById('sat_pct_value');"
    "if(s&&o)o.value=s.value+'%';})();</script>");

char s_rain_checkbox_attrs[32] = "type=\"checkbox\"";
WiFiManagerParameter s_param_rain("rain", "Rain radar overlay", "T", 2,
                                  s_rain_checkbox_attrs, WFM_LABEL_AFTER);

WiFiManagerParameter s_param_imagery_credits(
    "<div style=\"font-size:.8em;color:#666;margin:4px 0 10px\">"
    "Satellite: Sentinel-2 cloudless by EOX IT Services GmbH (contains "
    "modified Copernicus Sentinel data), CC BY-NC-SA 4.0. Rain radar: "
    "RainViewer (free for personal use). Both receive the radar location."
    "</div>");

constexpr char kOtaPasswordAttrs[] =
    "type=\"password\" autocomplete=\"new-password\" "
    "placeholder=\"leave blank to keep current\"";
WiFiManagerParameter s_param_ota_password(
    "ota_password", "OTA password (user: admin)", "", kOtaPasswordParamLen,
    kOtaPasswordAttrs);

void refreshCheckboxAttrs(char* attrs, size_t attrs_len, bool checked) {
  snprintf(attrs, attrs_len, "type=\"checkbox\"%s",
           checked ? " checked" : "");
}

void refreshPortalParamDefaults() {
  char lat_buf[kCoordParamLen + 1];
  char lon_buf[kCoordParamLen + 1];
  snprintf(lat_buf, sizeof(lat_buf), "%.6f", services::location::lat());
  snprintf(lon_buf, sizeof(lon_buf), "%.6f", services::location::lon());
  s_param_lat.setValue(lat_buf, kCoordParamLen);
  s_param_lon.setValue(lon_buf, kCoordParamLen);
  refreshCheckboxAttrs(s_miles_checkbox_attrs,
                       sizeof(s_miles_checkbox_attrs),
                       ui::radar::useMiles());
  s_param_miles.setValue("T", 2);
  refreshCheckboxAttrs(s_runways_checkbox_attrs,
                       sizeof(s_runways_checkbox_attrs),
                       ui::radar::showRunways());
  s_param_runways.setValue("T", 2);
  refreshCheckboxAttrs(s_footer_checkbox_attrs,
                       sizeof(s_footer_checkbox_attrs),
                       services::settings::footerEnabled());
  s_param_footer.setValue("T", 2);
  refreshCheckboxAttrs(s_weather_checkbox_attrs,
                       sizeof(s_weather_checkbox_attrs),
                       services::settings::weatherEnabled());
  s_param_weather.setValue("T", 2);
  refreshCheckboxAttrs(s_fahrenheit_checkbox_attrs,
                       sizeof(s_fahrenheit_checkbox_attrs),
                       services::settings::temperatureFahrenheit());
  s_param_fahrenheit.setValue("T", 2);
  refreshCheckboxAttrs(s_clock24_checkbox_attrs,
                       sizeof(s_clock24_checkbox_attrs),
                       services::settings::use24HourClock());
  s_param_clock24.setValue("T", 2);
  char text_scale_buf[kTextScaleParamLen + 1];
  snprintf(text_scale_buf, sizeof(text_scale_buf), "%d",
           services::settings::textScalePercent());
  s_param_text_scale.setValue(text_scale_buf, kTextScaleParamLen);
  refreshCheckboxAttrs(s_satellite_checkbox_attrs,
                       sizeof(s_satellite_checkbox_attrs),
                       services::settings::satelliteBackground());
  s_param_satellite.setValue("T", 2);
  char sat_pct_buf[kSatBrightnessParamLen + 1];
  snprintf(sat_pct_buf, sizeof(sat_pct_buf), "%d",
           services::settings::satelliteBrightnessPercent());
  s_param_sat_brightness.setValue(sat_pct_buf, kSatBrightnessParamLen);
  refreshCheckboxAttrs(s_rain_checkbox_attrs, sizeof(s_rain_checkbox_attrs),
                       services::settings::rainOverlay());
  s_param_rain.setValue("T", 2);
  s_param_ota_password.setValue("", kOtaPasswordParamLen);
}

void onPortalParamsSaved() {
  if (!services::location::saveFromStrings(s_param_lat.getValue(),
                                           s_param_lon.getValue())) {
    Serial.println("Invalid lat/lon in portal — keeping previous location");
  }
  ui::radar::saveMilesFromPortal(s_param_miles.getValue());
  ui::radar::saveRunwaysFromPortal(s_param_runways.getValue());
  services::settings::saveFromPortal(
      s_param_footer.getValue(), s_param_weather.getValue(),
      s_param_fahrenheit.getValue(), s_param_clock24.getValue(),
      s_param_text_scale.getValue(), s_param_satellite.getValue(),
      s_param_sat_brightness.getValue(), s_param_rain.getValue(),
      s_param_ota_password.getValue());
}

void savePortalParamsFromRequest(WebServer& web) {
  const String latitude = web.arg("radar_lat");
  const String longitude = web.arg("radar_lon");
  const String miles = web.arg("use_miles");
  const String runways = web.arg("show_runways");
  const String footer = web.arg("show_footer");
  const String weather = web.arg("show_weather");
  const String fahrenheit = web.arg("temp_f");
  const String clock24 = web.arg("clock_24");
  const String text_scale = web.arg("text_scale");
  const String satellite = web.arg("sat_bg");
  const String sat_brightness = web.arg("sat_pct");
  const String rain = web.arg("rain");
  const String ota_password = web.arg("ota_password");

  if (!services::location::saveFromStrings(latitude.c_str(),
                                           longitude.c_str())) {
    Serial.println("Invalid lat/lon in portal — keeping previous location");
  }
  ui::radar::saveMilesFromPortal(miles.c_str());
  ui::radar::saveRunwaysFromPortal(runways.c_str());
  services::settings::saveFromPortal(
      footer.c_str(), weather.c_str(), fahrenheit.c_str(), clock24.c_str(),
      text_scale.c_str(), satellite.c_str(), sat_brightness.c_str(),
      rain.c_str(), ota_password.c_str());
  refreshPortalParamDefaults();
}

void handleSettingsSaved() {
  if (!s_wm.server) {
    return;
  }

  WebServer& web = *s_wm.server;
  savePortalParamsFromRequest(web);
  web.send(
      200, "text/html",
      "<!doctype html><html lang='en'><head>"
      "<meta charset='utf-8'>"
      "<meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<meta http-equiv='refresh' content='3;url=/param'>"
      "<title>Setup saved</title>"
      "<style>body{font-family:verdana;text-align:center;margin:0;padding:3rem}"
      ".msg{display:inline-block;min-width:16rem;text-align:left;padding:1.5rem;"
      "border:1px solid #eee;border-left:5px solid #5cb85c;"
      "border-radius:.3rem}a{color:#1fa3ec}</style></head><body>"
      "<div class='msg'><strong>Saved</strong><br>"
      "<small>Returning to Setup in 3 seconds...</small><br><br>"
      "<a href='/param'>Return now</a></div></body></html>");
}

void attachSettingsRoutes() {
  if (!s_wm.server) {
    return;
  }
  // Register before WiFiManager's built-in /paramsave handler so the custom
  // confirmation can redirect back to Setup.
  s_wm.server->on("/paramsave", HTTP_POST, handleSettingsSaved);
  // Registered before WiFiManager's own /info, so this version wins.
  s_wm.server->on("/info", HTTP_GET, []() { s_wm.handleInfoPage(); });
  s_wm.server->on("/debug", HTTP_GET, []() {
    if (s_wm.server) {
      services::diag::handleRequest(*s_wm.server);
    }
  });
  s_wm.server->on("/screen.bmp", HTTP_GET, []() {
    if (s_wm.server) {
      services::diag::handleScreenshot(*s_wm.server);
    }
  });
}

void attachPortalParams(WiFiManager& wm) {
  refreshPortalParamDefaults();
  wm.addParameter(&s_param_lat);
  wm.addParameter(&s_param_lon);
  wm.addParameter(&s_param_map_picker);
  wm.addParameter(&s_param_miles);
  wm.addParameter(&s_param_runways);
  wm.addParameter(&s_param_footer);
  wm.addParameter(&s_param_weather);
  wm.addParameter(&s_param_fahrenheit);
  wm.addParameter(&s_param_clock24);
  wm.addParameter(&s_param_after_clock_break);
  wm.addParameter(&s_param_text_scale);
  wm.addParameter(&s_param_text_scale_output);
  wm.addParameter(&s_param_satellite);
  wm.addParameter(&s_param_after_satellite_break);
  wm.addParameter(&s_param_sat_brightness);
  wm.addParameter(&s_param_sat_brightness_output);
  wm.addParameter(&s_param_rain);
  wm.addParameter(&s_param_imagery_credits);
  wm.addParameter(&s_param_ota_password);
  wm.setSaveParamsCallback(onPortalParamsSaved);
}

void markForceConfigPortal() {
  s_force_config_portal = true;
  Preferences prefs;
  if (!prefs.begin(kWifiPrefsNamespace, false)) {
    return;
  }
  prefs.putBool(kPrefsForcePortalKey, true);
  prefs.end();
}

bool consumeForceConfigPortal() {
  if (s_force_config_portal) {
    s_force_config_portal = false;
    Preferences prefs;
    if (prefs.begin(kWifiPrefsNamespace, false)) {
      prefs.remove(kPrefsForcePortalKey);
      prefs.end();
    }
    return true;
  }

  Preferences prefs;
  if (!prefs.begin(kWifiPrefsNamespace, true)) {
    return false;
  }
  const bool pending = prefs.getBool(kPrefsForcePortalKey, false);
  prefs.end();
  if (!pending) {
    return false;
  }

  if (prefs.begin(kWifiPrefsNamespace, false)) {
    prefs.remove(kPrefsForcePortalKey);
    prefs.end();
  }
  return true;
}

bool storedWifiCredentials() {
  wifi_mode_t mode = WIFI_MODE_NULL;
  if (esp_wifi_get_mode(&mode) != ESP_OK || mode == WIFI_MODE_NULL) {
    WiFi.mode(WIFI_STA);
    delay(50);
  }

  wifi_config_t conf = {};
  if (esp_wifi_get_config(WIFI_IF_STA, &conf) != ESP_OK) {
    return false;
  }
  return conf.sta.ssid[0] != '\0';
}

void eraseWifiCredentials() {
  stopLanWebPortal();
  WiFi.setAutoReconnect(false);
  WiFi.mode(WIFI_OFF);
  delay(100);

  ensureWifiManager();
  WiFi.persistent(true);
  s_wm.resetSettings();
  s_wm.erase();
  WiFi.disconnect(true, true);
  WiFi.persistent(false);

  WiFi.mode(WIFI_OFF);
  delay(100);
}

void resetWifiCredentials() {
  markForceConfigPortal();
  eraseWifiCredentials();
  services::location::clear();
  ui::radar::unitsReset();
  services::settings::clear();
  Serial.println("WiFi credentials, location, units, and display settings cleared");
}

void onConfigPortalApStarted(WiFiManager*) {
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  statusScreenPortal();
#ifdef WM_MDNS
  if (MDNS.begin(config::kPortalHostname)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("Setup portal: http://%s.local (or http://%s)\n",
                  config::kPortalHostname, config::kPortalIp);
  } else {
    Serial.printf("Setup portal: http://%s (mDNS unavailable)\n", config::kPortalIp);
  }
#else
  Serial.printf("Setup portal: http://%s\n", config::kPortalIp);
#endif
}

bool wifiLinkUp() {
  return WiFi.status() == WL_CONNECTED &&
         WiFi.localIP() != IPAddress(0, 0, 0, 0);
}

void ensureWifiManager() {
  if (s_wm_configured) {
    return;
  }
  s_wm.setConfigPortalTimeout(config::kWifiPortalTimeoutSec);
  s_wm.setAPStaticIPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1),
                           IPAddress(255, 255, 255, 0));
  s_wm.setHostname(config::kPortalHostname);
  s_wm.setTitle("Plane Radar");
  s_wm.setAPCallback(onConfigPortalApStarted);
  attachPortalParams(s_wm);
  services::ota::configure(s_wm, attachSettingsRoutes);
  s_wm_configured = true;
}

void startLanWebPortal() {
  if (!wifiLinkUp() || s_wm.getWebPortalActive() ||
      s_wm.getConfigPortalActive()) {
    return;
  }
  refreshPortalParamDefaults();
  WiFi.mode(WIFI_STA);
  s_wm.setConfigPortalBlocking(false);
#ifdef WM_MDNS
  MDNS.end();
  if (MDNS.begin(config::kPortalHostname)) {
    MDNS.addService("http", "tcp", 80);
  }
#endif
  s_wm.startWebPortal();
  Serial.printf("LAN config: http://%s.local or http://%s\n",
                config::kPortalHostname, WiFi.localIP().toString().c_str());
}

void stopLanWebPortal() {
  if (!s_wm.getWebPortalActive()) {
    return;
  }
  s_wm.stopWebPortal();
#ifdef WM_MDNS
  MDNS.end();
#endif
}

void prepareSta() {
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(WIFI_PS_NONE);
  WiFi.setAutoReconnect(true);
}

void startStaConnect(const String& ssid, const String& pass) {
  prepareSta();
  if (ssid.length() > 0) {
    WiFi.begin(ssid.c_str(), pass.c_str());
  } else {
    WiFi.begin();
  }
}

bool waitForLinkWithUi(const char* ssid_for_ui, unsigned long attempt_ms) {
  const unsigned long deadline = millis() + attempt_ms;
  while (millis() < deadline) {
    if (wifiLinkUp()) {
      return true;
    }
    bootButtonPollLongPress();
    statusScreenConnectingTick();
    delay(config::kWifiConnectingFrameMs);
  }
  return wifiLinkUp();
}

bool tryConnectWithUi(const String& ssid, const String& pass, bool show_ui) {
  if (wifiLinkUp()) {
    return true;
  }

  const char* ui_ssid = ssid.length() > 0 ? ssid.c_str() : "network";
  if (show_ui) {
    statusScreenConnectingBegin(ui_ssid);
  }

  for (uint8_t attempt = 1; attempt <= config::kWifiConnectAttempts; ++attempt) {
    if (attempt > 1) {
      Serial.printf("WiFi connect retry %u/%u\n", attempt,
                    config::kWifiConnectAttempts);
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
      delay(400);
    }

    startStaConnect(ssid, pass);

    if (waitForLinkWithUi(ui_ssid, config::kWifiConnectAttemptMs)) {
      return true;
    }
  }

  return false;
}

bool connectSavedNetwork(bool show_ui) {
  if (!storedWifiCredentials()) {
    return false;
  }

  ensureWifiManager();
  const String ssid = s_wm.getWiFiSSID();
  if (ssid.length() == 0) {
    return false;
  }
  const String pass = s_wm.getWiFiPass();
  return tryConnectWithUi(ssid, pass, show_ui);
}

bool openConfigPortal() {
  stopLanWebPortal();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(50);
  statusScreenPortal();
  s_wm.setConfigPortalBlocking(false);
  s_wm.startConfigPortal(config::kPortalApName);
  while (s_wm.getConfigPortalActive()) {
    bootButtonPollLongPress();
    if (s_wm.process()) {
      return true;
    }
    delay(10);
  }
  return wifiLinkUp();
}

}  // namespace

bool wifiShowsSetupScreenOnBoot() {
  if (s_force_config_portal) {
    return true;
  }
  Preferences prefs;
  if (!prefs.begin(kWifiPrefsNamespace, true)) {
    return false;
  }
  const bool pending = prefs.getBool(kPrefsForcePortalKey, false);
  prefs.end();
  return pending;
}

bool wifiBootButtonPressed() {
  return digitalRead(config::kBootPin) == LOW;
}

void bootButtonInit() { initBootButton(); }

bool bootButtonConsumeTap() {
  portENTER_CRITICAL(&s_boot_mux);
  const bool tap = s_boot_tap_pending;
  if (tap) {
    s_boot_tap_pending = false;
  }
  portEXIT_CRITICAL(&s_boot_mux);
  return tap;
}

void bootButtonPollLongPress() {
  if (wifiBootButtonPressed()) {
    portENTER_CRITICAL(&s_boot_mux);
    if (!s_boot_is_down) {
      s_boot_is_down = true;
      s_boot_down_ms = millis();
    }
    const unsigned long down_ms = s_boot_down_ms;
    portEXIT_CRITICAL(&s_boot_mux);

    if (!s_long_press_handled &&
        millis() - down_ms >= config::kBootResetHoldMs) {
      s_long_press_handled = true;
      Serial.println("BOOT held — resetting WiFi");
      wifiResetCredentialsAndReboot();
    }
  } else {
    portENTER_CRITICAL(&s_boot_mux);
    s_boot_is_down = false;
    portEXIT_CRITICAL(&s_boot_mux);
    s_long_press_handled = false;
  }
}

void wifiResetCredentialsAndReboot() {
  resetWifiCredentials();
  statusScreenWifiReset();
  delay(800);
  esp_restart();
}

bool wifiReconnect() {
  initBootButton();
  Serial.println("WiFi reconnecting...");
  return connectSavedNetwork(true);
}

void wifiLoop() {
  ensureWifiManager();
  if (wifiLinkUp()) {
    if (!s_wm.getWebPortalActive() && !s_wm.getConfigPortalActive()) {
      startLanWebPortal();
    }
    if (s_wm.getWebPortalActive() || s_wm.getConfigPortalActive()) {
      bootButtonPollLongPress();
      s_wm.process();
    }
  } else {
    stopLanWebPortal();
  }
}

bool wifiSetupConnect() {
  initBootButton();
  ensureWifiManager();

  const bool force_portal = consumeForceConfigPortal();
  WiFi.setAutoReconnect(false);

  if (force_portal) {
    eraseWifiCredentials();
    WiFi.mode(WIFI_OFF);
    delay(100);
  }

  if (force_portal) {
    Serial.println("Opening WiFi setup portal (after reset)");
    if (openConfigPortal() && wifiLinkUp()) {
      WiFi.setAutoReconnect(true);
      Serial.printf("Connected: %s  IP %s\n", WiFi.SSID().c_str(),
                    WiFi.localIP().toString().c_str());
      return true;
    }
    Serial.println("WiFi connection failed");
    statusScreenConnectFailed();
    return false;
  }

  Serial.println("Connecting to WiFi (portal opens if needed)...");

  if (wifiLinkUp()) {
    WiFi.setAutoReconnect(true);
    Serial.printf("Connected: %s  IP %s\n", WiFi.SSID().c_str(),
                  WiFi.localIP().toString().c_str());
    return true;
  }

  if (storedWifiCredentials() && connectSavedNetwork(true)) {
    WiFi.setAutoReconnect(true);
    Serial.printf("Connected: %s  IP %s\n", WiFi.SSID().c_str(),
                  WiFi.localIP().toString().c_str());
    return true;
  }

  if (storedWifiCredentials()) {
    Serial.println("Saved WiFi could not connect — opening setup portal");
  } else {
    Serial.println("No saved WiFi — opening setup portal");
  }

  if (openConfigPortal() && wifiLinkUp()) {
    WiFi.setAutoReconnect(true);
    Serial.printf("Connected: %s  IP %s\n", WiFi.SSID().c_str(),
                  WiFi.localIP().toString().c_str());
    return true;
  }

  Serial.println("WiFi connection failed");
  statusScreenConnectFailed();
  return false;
}
