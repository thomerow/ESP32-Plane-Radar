# Plane Radar

<img width="800" height="450" alt="plane-radar" src="https://github.com/user-attachments/assets/716d0992-dab8-47ba-8f1a-2aec7f607419" />

**3D printed case (STL + assembly):** [MakerWorld](https://makerworld.com/en/models/2872376-esp32-plane-radar-live-ads-b-on-a-round-display#profileId-3207083) · **Firmware:** [Releases](../../releases)

Firmware for an **ESP32-C3 Super Mini** and a **1.28″ round GC9A01** display (240×240). Shows a circular **ADS-B radar** around your configured location, with flight routes, detailed aircraft models, local weather/time, browser settings, and authenticated OTA updates.

## What it does

1. **Wi‑Fi setup** (if needed) — captive portal on AP **`PlaneRadar-Setup`**
2. **Radar** — live aircraft from [adsb.fi](https://opendata.adsb.fi/) on a sonar-style grid
3. **Useful labels** — route (for example `BOS-IND`), detailed aircraft model, and altitude
4. **Readable footer** — current conditions, temperature, humidity, local time, and date

After Wi‑Fi is saved, the device reconnects automatically; the radar runs in the main loop with periodic ADS-B updates (~3 s).

## Controls (BOOT, GPIO 9, active LOW)

| Action | Effect |
|--------|--------|
| **Short tap** | Cycle range preset (5 → 10 → 15 → 25 km); saved to flash |
| **Hold 3 s** | Factory-reset Wi‑Fi, location, units, display settings, and OTA password; reboot into setup portal |

The 3 s hold works at any time the firmware is running, including on the **Connecting** screen — useful if saved credentials no longer work. Do not hold BOOT while powering up: GPIO9 low at reset puts the ESP32‑C3 into its ROM download mode and the firmware never starts.

## Wi‑Fi setup portal

**First-time setup** (no saved Wi‑Fi):

1. Connect to **`PlaneRadar-Setup`**
2. Open **`http://plane-radar.local`** (preferred) or **`http://192.168.4.1`** — both are shown on the yellow setup screen; captive portal may open automatically
3. Set home Wi‑Fi, then save

**Reconfigure anytime** (after the device is on your network):

1. Open **`http://plane-radar.local`** or **`http://<device-ip>`** (e.g. from your router or serial log at boot)
2. Choose **Setup**
3. Change coordinates (by hand or on the map), display options, units, or OTA password; save

> **The portal is not password-protected.** Only the firmware-update routes (`/firmware`, `/update`) ask for a password. Anyone who can reach the device on your network can open **Setup**, read the Wi‑Fi page (which lists nearby networks), view **Diagnostics** (position and tracked aircraft) and change the configuration. The portal starts by itself whenever Wi‑Fi is up. Treat the device as trusted-LAN-only.

The same portal runs on the setup AP and on the device’s LAN IP while connected to Wi‑Fi. mDNS hostname is `plane-radar` → **plane-radar.local** (`kPortalHostname` in `config.h`). Some clients resolve `.local` slowly; use the IP if needed.

Changing coordinates no longer requires a credential reset. The new position is validated in the browser and firmware, saved to NVS, and used by the next aircraft/weather refresh.

### Picking the location on a map

**Setup** shows an OpenStreetMap picker under the coordinate fields. Click the
map or drag the marker to set the radar center, or type a place into the search
box (**Find**) to jump there — the latitude and longitude fields follow along.
Editing a field by hand still works and moves the marker to match. Nothing is
stored until you press **Save**.

Leaflet and the map tiles are loaded by your browser from a CDN, so the picker
only appears when **your browser** has internet access — that is, when you
reconfigure the device over your LAN (`http://plane-radar.local`). On the
first-time setup AP `PlaneRadar-Setup` there is no internet, so the map hides
itself, notes that it needs a connection, and you enter the coordinates
manually. The device itself never fetches map data.

**Custom fields** (stored in NVS):

| Field | Purpose |
|-------|---------|
| **Latitude / Longitude** | Radar center and ADS-B query position (defaults in `config.h` until set); settable by hand or on the map picker |
| **Display distances in miles** | Ring scale label in **mi** instead of **km** (e.g. `6mi` vs `10km`) |
| **Show airport runways** | Major-airport runway overlay on the radar (off to hide) |
| **Show weather and clock** | Enables/disables the complete bottom footer |
| **Show current weather** | Shows current condition, temperature, and humidity |
| **Temperature in Fahrenheit** | Uses °F instead of °C |
| **Use 24-hour clock** | Uses 24-hour instead of compact 12-hour time |
| **Radar text size (%)** | Scales radar labels and footer text from 80–130% in steps of 5; default is 110% |
| **Satellite background** | Sentinel-2 satellite image of the visible area instead of the dark radar background; hides rings, crosshairs and the range label (see below) |
| **Satellite image brightness (%)** | Dims the satellite image from 20–100% in steps of 5 so symbols and tags stay readable; default is 100% |
| **Rain radar overlay** | Semi-transparent RainViewer precipitation layer, in both background modes |
| **OTA password** | Password for firmware uploads; username is `admin`. Leave blank to keep the current one |

After a reset, the device reboots and shows the setup screen immediately (no “Connecting” loop on stale credentials).

## Diagnostics page

**Setup → Diagnostics**, or `http://plane-radar.local/debug` directly. A
read-only status page that reports what the firmware is actually computing,
rather than what it should be computing:

| Section | Shows |
|---------|-------|
| **Radar and projection** | Center, active range preset, fetch radius, pixels per km, the `cos(latitude)` longitude scale, and whether the 115 KB frame sprite got its memory |
| **Aircraft** | Every tracked aircraft with its raw position, the east/north offset in km, distance, the exact screen pixel it is drawn at, and whether it renders as a symbol or a rim dot |
| **Airports** | Runway segments in range with their screen endpoints |
| **ADS-B service** | Fetch and enrichment counters, last error code, cache usage |
| **Weather and time** | The formatted footer strings, even when the footer is switched off |
| **Network** | SSID, RSSI, channel, IP, MAC, TX power |
| **System** | Build date, **sketch MD5**, running OTA partition, uptime, reset reason, chip, temperature |
| **Memory** | Free heap, largest contiguous block, lowest free ever |

The **Radar and projection** section also reports the background mode, whether
the satellite image for the current view is cached and how long it took to
decode, the age and rain coverage of the current rain radar frame, and how much
of the LittleFS image cache is in use.

**`http://plane-radar.local/screen.bmp`** returns the last rendered frame as a
240×240 BMP, colored the way the panel shows it — handy for checking a layout
change without photographing the display.

Two of these are worth knowing about:

**Sketch MD5** equals the md5sum of `.pio/build/supermini/firmware.bin`, so you
can tell at a glance whether the build you just made is the one running — the
`Build date` field cannot do that reliably, because it comes from a library
translation unit that is not always recompiled.

**Largest free block** is the number that explains failed HTTPS calls. A TLS
handshake needs a large contiguous allocation, and this board has no PSRAM. When
that figure falls well under ~40 KB, ADS-B or weather fetches start failing with
`start_ssl_client: -1` in the serial log even though the free-heap total still
looks comfortable. The frame sprite (240×240 at 16 bpp = 115 KB) is allocated
once and held for the life of the process, which is what keeps the ceiling low.

The page streams itself in ~1 KB chunks instead of building the whole document
in RAM, and costs nothing when nobody requests it. Like the rest of the portal
it has **no password**.

## Radar display

### Grid

- Dark blue background, subdued green rings and crosshairs
- White **N / S / E / W** at the bezel; range label on the **east** spoke, drawn just inside the outer ring (the value is the ring‑3 distance = ¾ of outer radius)
- White center dot

Layout and colors: `include/ui/radar_theme.h`.

### Satellite background and rain radar

Both are off by default and switched on under **Setup**.

**Satellite background.** The background becomes a cloud-free Sentinel-2 image
of exactly the visible area. The grid, crosshairs and range label are left out;
aircraft triangles, speed vectors, rim dots, runway lines and the center dot get
a dark outline, and every label sits on a semi-transparent dark backdrop so it
stays readable on any terrain. Tag backdrops are all drawn before any tag text,
so overlapping tags never dim each other.

- Source: EOX **Sentinel-2 cloudless 2025** via WMS, requested in EPSG:4326 with
  a bounding box of ±half-width/111 km in latitude and ±half-width/(111 km ·
  cos lat) in longitude — the same flat projection the radar uses, so no tiles
  are stitched and nothing is reprojected on the device.
- One 240×240 JPEG (~15–20 KB) per range preset, downloaded the first time a
  range is shown at a location and cached in LittleFS on the `spiffs`
  partition. Changing the location refetches lazily.
- The frame RAM has no room for a second 115 KB buffer, so the JPEG is decoded
  into the frame sprite on every redraw (~110 ms) and dimmed in the same pass.

**Rain radar overlay.** The newest RainViewer frame (updated every 10 minutes,
checked every 5) is blended over the background, cyan for light rain through
yellow to magenta for heavy rain; light rain stays almost transparent. Frames
older than 40 minutes are hidden.

- Source: RainViewer Weather Maps API, free tier — **zoom 7 at most**, which is
  roughly 0.75 km per pixel in central Europe. That matches the native
  resolution of the radar data, but the edges look soft or blocky at the
  5 km preset.
- One 256×256 PNG centered on the radar location covers every range preset, so
  switching ranges only re-decodes the cached file.
- Decoding needs a 32 KB inflate window that the heap cannot spare: the decoder
  uses the ROM `tinfl` and borrows the frame sprite's buffer as scratch, then
  stores a small 80×80 intensity grid as a file and streams it while drawing.
  Nothing stays allocated, so HTTPS for ADS-B and weather keeps working.

Attribution: *Sentinel-2 cloudless — https://s2maps.eu by EOX IT Services GmbH
(Contains modified Copernicus Sentinel data 2025)*, CC BY-NC-SA 4.0. Rain radar
by [RainViewer](https://www.rainviewer.com/), free for personal and educational
use.

### Range presets

| Ring 3 label | Outer radius (aircraft scale) |
|------------|-------------------------------|
| 5 km / 3 mi | ~6.7 km |
| 10 km / 6 mi | ~13.3 km (default) |
| 15 km / 9 mi | ~20 km |
| 25 km / 16 mi | ~33.3 km |

Preset and miles/km choice persist across reboot (`planeradar` NVS namespace).

### Runways

- Major airports from OurAirports (`large_airport`); all open runway strips in range (helipads excluded)
- Teal runway lines with one ICAO label per airport (e.g. `KJFK`); toggle in the Wi‑Fi setup portal
- The label sits just below the airport's runways (above them for airports north of the center), so it never covers the lines
- Update the embedded list: `python3 scripts/build_large_airports.py`

### Aircraft

- **Inside the outer ring** — red heading triangle, magenta speed vector (clipped at the ring), route / detailed type / altitude tags
- **Beyond ~88 % of the outer radius** (still within ADS-B fetch) — small **red dot on the screen rim** at the correct bearing (direction cue; not distance-accurate past the ring)
- **Tags** — placed toward the **center**: west (left) → tag on the **right** of the symbol; east (right) → tag on the **left**
- **Route fallback** — the callsign is shown until route data arrives, and remains the fallback when no route is known
- **Detailed type** — aircraft data is compacted for the display (for example `Boeing 737-800` → `B737-800`)

As range increases (or aircraft approach), targets move inward; rim dots become full symbols once they are inside ~88 % of the outer radius.

Origin/destination is not transmitted in ADS-B messages. The firmware enriches each active callsign through [ADSBDB](https://www.adsbdb.com/), rate-limits lookups, and caches successful results for six hours (misses for ten minutes). It first requests aircraft and route together, then retries the callsign-only endpoint whenever that returned no route. Route databases are based on known/scheduled callsigns, so private, repositioning, diverted, or recently changed flights may still have no route or an imperfect match.

### Weather and time

The bottom overlay uses the radar coordinates. Current conditions come from [Open-Meteo](https://open-meteo.com/) every 15 minutes; its location timezone offset and NTP provide the clock. The two rows use plain-language conditions and label relative humidity (`RH`):

```text
OVERCAST 82F RH100%
21:45 25 JUL
```

Disable just the weather row or the complete footer in **Setup**. Radar and
footer text defaults to 110% and can be adjusted from 80–130% in the same page.

### ADS-B

- Source: `https://opendata.adsb.fi/api/v3/`
- Route and aircraft enrichment: `https://api.adsbdb.com/v0/`
- Fetch radius: `ui::radar::fetchRadiusKm()` — scales with the active preset to roughly the screen edge (so rim dots have data)
- Poll interval: `kAdsbFetchIntervalMs` (3 s) in `config.h`
- Ground aircraft hidden by default (`kAdsbShowGroundAircraft`)
- Hard caps: 64 aircraft per fetch, 32 airport labels per frame, 48 cached enrichment results
- Wi‑Fi TX power is capped at 8.5 dBm in both AP and STA mode (`wifi_setup.cpp`) to keep weak USB supplies stable — raise it if range matters more than brown-out margin

The device sends the configured coordinates to adsb.fi and Open-Meteo, and sends active callsign/Mode-S identifiers to ADSBDB. With the satellite background or rain overlay enabled, the coordinates also go to EOX (tiles.maps.eox.at) or RainViewer. These calls use HTTPS but **do not verify the server certificate** (`setInsecure()`) — there is no room for a certificate bundle, so the traffic is encrypted but not authenticated.

While the **Setup** page is open, your browser — not the device — additionally loads Leaflet from cdnjs, map tiles from openstreetmap.org, and, if you use the search box, sends that query to Nominatim.

## Configuration

Edit **`include/config.h`** for hardware and behavior:

| Area | Keys / notes |
|------|----------------|
| Portal | `kPortalApName`, `kPortalIp`, `kPortalHostname` / `kPortalHostUrl` (mDNS; needs `-DWM_MDNS` in `platformio.ini`) |
| Wi‑Fi timing | connect attempts, reconnect grace, portal timeout (`0` = no timeout) |
| BOOT | `kBootPin`, `kBootResetHoldMs`, `kBootTapMinMs` |
| Display SPI | pins, `kDisplayInvert`, `kDisplayRgbOrder`, `kDisplaySpiWriteHz` |
| Default location | `kDefaultRadarLat`, `kDefaultRadarLon` (until portal overrides) |
| ADS-B | `kAdsbApiBase`, `kAdsbFetchIntervalMs`, `kAdsbShowGroundAircraft` |
| Flight enrichment | lookup interval, timeout, and cache durations |
| Weather | endpoint, request timeout, and refresh interval |
| Defaults | initial OTA credentials |

Range presets: `include/ui/radar_range.h` (`kRangePresets`).

## Project layout

```
include/
  config.h
  hardware/
    lgfx_config.hpp
    display.h
    display_font.h
  data/
    large_airports.h
  ui/
    radar_theme.h
    radar_range.h
    radar_display.h
    pixel_ops.h            — direct work on the frame sprite's RGB565 buffer
    runway_overlay.h
    status_screens.h
  services/
    wifi_setup.h
    radar_location.h
    adsb_client.h
    diagnostics.h
    display_settings.h
    file_cache.h           — LittleFS image cache, HTTPS download to file
    ota_update.h
    rain_radar.h
    satellite_map.h
    weather_time.h
data/
  ui_font.vlw              — embedded smooth UI font (Noto Sans Bold)
partitions/
  plane_radar.csv          — dual-OTA 4 MB layout; the 384 KB `spiffs` partition holds the LittleFS image cache
scripts/
  build_large_airports.py  — regenerates the embedded airport/runway tables
  merge_firmware.py        — post-build hook: `pio run -t merge`
  merge-firmware.sh        — writes release/plane-radar-merged.bin
src/
  main.cpp
  data/
    large_airports_data.cpp
  hardware/
    display.cpp
    display_font.cpp
  ui/
    radar_display.cpp
    radar_range.cpp
    runway_overlay.cpp
    status_screens.cpp
  services/
    wifi_setup.cpp
    radar_location.cpp
    adsb_client.cpp
    display_settings.cpp
    diagnostics.cpp
    file_cache.cpp
    ota_update.cpp
    rain_radar.cpp
    satellite_map.cpp
    weather_time.cpp
platformio.ini
.github/workflows/
  build.yml                — build on push/PR
  release.yml              — attach firmware to a tagged release
LICENSE
```

## Wiring (GC9A01 ↔ ESP32-C3 Super Mini)

| Display | ESP32-C3 |
|---------|----------|
| VCC | 3V3 |
| GND | GND |
| RST | GPIO **0** |
| CS | GPIO **1** |
| DC | GPIO **10** |
| SDA (MOSI) | GPIO **3** |
| SCL (SCLK) | GPIO **4** |
| BOOT (user) | GPIO **9** |

## Build

```bash
pio run -t upload
pio device monitor
```

- PlatformIO env: **`supermini`**
- Serial: **115200** baud
- USB CDC on boot enabled in `platformio.ini` for the Super Mini

### Web-flashable release image

Single `.bin` for [esptool-js](https://espressif.github.io/esptool-js/) and similar tools (ESP32-C3, 4 MB, flash at **0x0**):

```bash
chmod +x scripts/merge-firmware.sh   # once
./scripts/merge-firmware.sh
```

Writes `release/plane-radar-merged.bin`. Skip rebuild if firmware is already built:

```bash
./scripts/merge-firmware.sh --no-build
```

Or via PlatformIO only (output: `.pio/build/supermini/firmware-merged.bin`):

```bash
pio run -e supermini
pio run -t merge -e supermini
```

Put the board in download mode (hold **BOOT**, tap **RESET**), then flash with Chrome/Edge over USB.

### OTA firmware updates

The firmware uses two 1.75 MB application slots. After the OTA-capable partition table is installed, updates can be uploaded without USB:

1. Open `http://plane-radar.local`
2. Choose **Firmware update**
3. Sign in with username `admin` and your configured OTA password
4. Upload the release file ending in **`-ota.bin`** (or PlatformIO's `.pio/build/supermini/firmware.bin`)
5. Keep power connected while the device writes flash and restarts

The initial password is **`plane-radar`**. Change it under **Setup** before using the device on a shared network.

> **One-time migration:** firmware built with the old single-app partition cannot install this new partition table through app-only OTA. Flash the new **`-full.bin`**/merged image over USB once. Every later update can use the OTA image.

Never upload the merged/full image to the OTA form; it contains the bootloader and partition table and is only for flashing at offset `0x0`.

### CI and releases (GitHub Actions)

| Workflow | When | Output |
|----------|------|--------|
| [Build](.github/workflows/build.yml) | Push to `main`/`master`, any PR, manual | Artifact `plane-radar-supermini` (merged + split `.bin` files, ~90 days) |
| [Release](.github/workflows/release.yml) | Git tag `v*` (e.g. `v1.0.0`) | GitHub Release `-full.bin` and `-ota.bin` assets + checksums |

To ship a version users can download:

```bash
git tag v1.0.0
git push origin v1.0.0
```

The release workflow attaches both images. Use `-full.bin` at offset `0x0` for first install/recovery and `-ota.bin` in the device's authenticated firmware page.

## Dependencies

- [LovyanGFX](https://github.com/lovyan03/LovyanGFX)
- [WiFiManager](https://github.com/tzapu/WiFiManager)
- [ArduinoJson](https://github.com/bblanchon/ArduinoJson)

Runtime data services:

- [adsb.fi](https://opendata.adsb.fi/) — nearby aircraft
- [ADSBDB](https://www.adsbdb.com/) — route and detailed aircraft data
- [Open-Meteo](https://open-meteo.com/) — current weather and local timezone offset
