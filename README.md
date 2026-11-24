# Plane Radar

<img width="800" height="450" alt="plane-radar" src="https://github.com/user-attachments/assets/716d0992-dab8-47ba-8f1a-2aec7f607419" />

**3D printed case (STL + assembly):** [MakerWorld](https://makerworld.com/en/models/2872376-esp32-plane-radar-live-ads-b-on-a-round-display#profileId-3207083) · **Firmware:** [Releases](../../releases)

Firmware for an **ESP32-C3 Super Mini** and a **1.28″ round GC9A01** display (240×240). Shows a circular **ADS-B radar** around your configured location, with flight routes, detailed aircraft models, an optional **satellite background** and **rain radar overlay**, local weather/time, browser settings, and authenticated OTA updates.

## What it does

1. **Wi‑Fi setup** (if needed) — captive portal on AP **`PlaneRadar-Setup`**
2. **Radar** — live aircraft from [adsb.fi](https://opendata.adsb.fi/) on a sonar-style grid, or on a satellite image of the area
3. **Useful labels** — route (for example `BOS-IND`), detailed aircraft model, and altitude, placed so they avoid each other
4. **Early warning** — dots on the screen rim announce aircraft before they enter the view
5. **Optional extras** — rain radar overlay, and a footer with current conditions, temperature, humidity, local time, and date

After Wi‑Fi is saved, the device reconnects automatically; the radar runs in the main loop with periodic ADS-B updates (~3 s).

## Flashing the firmware

You do not need to program anything to put the current firmware on a device.
Pick the case that matches yours:

| Your situation | What to use | Wi‑Fi and settings |
|----------------|-------------|--------------------|
| The device already runs this firmware and is on your Wi‑Fi | **A. Update in the browser** with the `-ota.bin` file | kept |
| New board, very old firmware, or the device does not start | **B. Flash over USB** with the `-full.bin` file | **erased** — set up again afterwards |
| You want a version that has no release yet, or want to change the code | **C. Build from source** | kept |

**Where the files come from:** the [Releases](../../releases) page. Each release
has `plane-radar-<version>-full.bin` and `plane-radar-<version>-ota.bin`, each
with a `.sha256` checksum. A release can lag behind the newest code; the latest
build of the `main` branch is also available as the artifact
`plane-radar-supermini` on the [Actions](../../actions) page (needs a GitHub
login; there `firmware-merged.bin` is the full image and `firmware.bin` the OTA
image). Otherwise use path C.

### A. Update in the browser (OTA)

1. Open `http://plane-radar.local` (or the device's IP address from your router)
2. Choose **Firmware update**
3. Sign in as `admin` with your OTA password (initially **`plane-radar`**)
4. Upload the **`-ota.bin`** file — never the `-full.bin`
5. Keep the power connected; the device writes the update and restarts
6. To check: **Setup → Info** (or `/debug`) shows the running firmware
   version, e.g. *Plane Radar 2.1.0* (v2.0.0 does not show one yet)

### B. Flash over USB (full image)

Needs a USB‑C cable that carries data (some are charge-only) and Chrome or Edge
on a desktop computer. Nothing has to be installed: Windows 10/11, macOS and
Linux recognise the ESP32‑C3's built-in USB port without a driver.

1. **Put the board in download mode:** unplug it, **hold the BOOT button**,
   plug the USB cable in, then release BOOT. (Alternatively, with the cable in:
   hold BOOT, press and release RESET, release BOOT.) The display stays dark or
   shows nothing meaningful — that is expected.
2. Open [esptool-js](https://espressif.github.io/esptool-js/), click
   **Connect** and choose the port (shown as *USB JTAG/serial debug unit* or
   *USB Serial Device*; if several appear, unplug the board to see which one
   disappears).
3. Under **Program**, set the address to **`0x0`**, choose the **`-full.bin`**
   file, and click **Program**.
4. When it is done, unplug and replug the board. It starts with the cyan
   setup screen; continue with [Wi‑Fi setup portal](#wifi-setup-portal).

The full image contains bootloader, partition table and application, and it
overwrites the area where Wi‑Fi credentials and settings live — that is why
they are gone afterwards.

**Facts for a helper or AI assistant:** chip **ESP32‑C3**, 4 MB flash; full
image written at offset **`0x0`**; OTA image is app-only (two 1.75 MB OTA
slots, partition table in `partitions/plane_radar.csv`); native USB
Serial/JTAG, so no USB-UART driver; command-line equivalent of step 3:
`esptool.py --chip esp32c3 --port <port> write_flash 0x0 plane-radar-<version>-full.bin`
(`pip install esptool`). Serial log at 115200 baud; its first line is
`Plane Radar <version>`.

### C. Build from source

1. Install [Git](https://git-scm.com/) and [Python 3](https://www.python.org/),
   then PlatformIO: `pip install platformio` (or install
   [VS Code](https://code.visualstudio.com/) with the *PlatformIO IDE*
   extension, which brings it along).
2. Get the code: `git clone https://github.com/thomerow/ESP32-Plane-Radar.git`
   (or *Code → Download ZIP* on GitHub and unpack it), then open a terminal in
   that folder.
3. Connect the board by USB and run:

   ```bash
   pio run -e supermini -t upload
   ```

   If the terminal does not know `pio`, use `python -m platformio run -e supermini -t upload`
   instead. The first run downloads the compiler and libraries (a few hundred
   MB, several minutes). The upload normally switches the board into download mode by
   itself; if it cannot connect, use the BOOT-button method from path B.
   This writes bootloader, partition table and app at their own addresses and
   keeps Wi‑Fi and settings.
4. Optional: watch the log with `pio device monitor` (115200 baud). Closing the
   monitor resets the board, which is harmless.

If the project folder is synced by Dropbox/OneDrive, a build can fail with
`unable to rename ... File exists`; just run it again (or build outside the
synced folder).

> **One-time migration:** firmware built with the old single-app partition cannot install this partition table through an OTA update. Flash the `-full.bin` over USB once (path B); every later update can use path A.

## Controls (BOOT, GPIO 9, active LOW)

| Action | Effect |
|--------|--------|
| **Short tap** | Cycle range preset (5 → 10 → 15 → 25 km); saved to flash |
| **Hold 3 s** | Factory-reset Wi‑Fi, location, units, display settings, and OTA password; reboot into setup portal |

The 3 s hold works at any time the firmware is running, including on the **Connecting** screen — useful if saved credentials no longer work. Do not hold BOOT while powering up: GPIO9 low at reset puts the ESP32‑C3 into its ROM download mode and the firmware never starts (which is exactly what [flashing over USB](#b-flash-over-usb-full-image) uses).

## Wi‑Fi setup portal

**First-time setup** (no saved Wi‑Fi):

1. Connect to **`PlaneRadar-Setup`**
2. Open **`http://plane-radar.local`** (preferred) or **`http://192.168.4.1`** — both are shown on the cyan setup screen; captive portal may open automatically
3. Set home Wi‑Fi, then save

**Reconfigure anytime** (after the device is on your network):

1. Open **`http://plane-radar.local`** or **`http://<device-ip>`** (e.g. from your router or serial log at boot)
2. Choose **Setup**
3. Change coordinates (by hand or on the map), display options, units, satellite/rain options, or OTA password; save

> **The portal is not password-protected.** Only the firmware-update routes (`/firmware`, `/update`) ask for a password. Anyone who can reach the device on your network can open **Setup**, read the Wi‑Fi page (which lists nearby networks), view **Diagnostics** (position and tracked aircraft) and change the configuration. The portal starts by itself whenever Wi‑Fi is up. Treat the device as trusted-LAN-only.

The same portal runs on the setup AP and on the device’s LAN IP while connected to Wi‑Fi. mDNS hostname is `plane-radar` → **plane-radar.local** (`kPortalHostname` in `config.h`). Some clients resolve `.local` slowly; use the IP if needed.

Changing coordinates does not require a credential reset. The new position is validated in the browser and firmware, saved to NVS, and used by the next aircraft/weather refresh.

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

**Custom fields** (stored in NVS; defaults apply to a freshly flashed device):

| Field | Purpose | Default |
|-------|---------|---------|
| **Latitude / Longitude** | Radar center and ADS-B query position; settable by hand or on the map picker | `config.h` |
| **Display distances in miles** | Ring scale label in **mi** instead of **km** (e.g. `6mi` vs `10km`) | off (km) |
| **Show airport runways** | Major-airport runway overlay on the radar | on |
| **Show weather and clock** | Enables/disables the complete bottom footer | off |
| **Show current weather** | Shows current condition, temperature, and humidity in the footer | off |
| **Temperature in Fahrenheit** | Uses °F instead of °C | off (°C) |
| **Use 24-hour clock** | Uses 24-hour instead of compact 12-hour time | on |
| **Radar text size (%)** | Scales radar labels and footer text from 80–130% in steps of 5 | 110% |
| **Satellite background** | Sentinel-2 satellite image of the visible area instead of the dark radar background; hides rings, crosshairs and the range label (see below) | off |
| **Satellite image brightness (%)** | Dims the satellite image from 20–100% in steps of 5 so symbols and tags stay readable | 100% |
| **Rain radar overlay** | Semi-transparent RainViewer precipitation layer, in both background modes | off |
| **OTA password** | Password for firmware uploads; username is `admin`. Leave blank to keep the current one | `plane-radar` |

After a reset, the device reboots and shows the setup screen immediately (no “Connecting” loop on stale credentials).

## Info page

**Setup → Info** (`/info`) is WiFiManager's info page, served by the firmware
so it matches this device: its **Available pages** table lists the pages that
actually exist here (`/param`, `/debug`, `/screen.bmp`, `/firmware`, …) instead
of the library's stock list, whose `/u` link no longer leads anywhere. The
sketch size is shown against the running app partition, the chip temperature in
°C only, the mDNS name next to the DHCP hostname, and the build date is replaced
by the firmware version and the sketch MD5 (the stock date comes from a library
file that is not always recompiled).

## Diagnostics page

**Setup → Diagnostics**, or `http://plane-radar.local/debug` directly. A
read-only status page that reports what the firmware is actually computing,
rather than what it should be computing:

| Section | Shows |
|---------|-------|
| **Radar and projection** | Center, active range preset, view and fetch radius, background mode, satellite image and rain radar status, image cache usage, pixels per km, the `cos(latitude)` longitude scale, and whether the 115 KB frame sprite got its memory |
| **Aircraft** | Every tracked aircraft with its raw position, the east/north offset in km, distance, the exact screen pixel it is drawn at, and whether it renders as a symbol or a rim dot |
| **Airports** | Runway segments in range with their screen endpoints |
| **ADS-B service** | Fetch and enrichment counters, last error code, TLS recovery reboots, aircraft in the last response, cache usage |
| **Weather and time** | The formatted footer strings, even when the footer is switched off |
| **Network** | SSID, RSSI, channel, IP, MAC, TX power |
| **System** | Firmware version, build time, **sketch MD5**, running OTA partition, uptime, reset reason, chip, temperature |
| **Memory** | Free heap, largest contiguous block, lowest free ever, loop-task stack headroom |

**`http://plane-radar.local/screen.bmp`** returns the last rendered frame as a
240×240 BMP, colored the way the panel shows it — handy for checking a layout
change without photographing the display.

Worth knowing:

**Firmware version** comes from the git tag at build time
(`scripts/firmware_version.py`): `2.1.0` for a release, `2.1.0-3-gabc1234` for
a build three commits after it, with `-dirty` when it was built with
uncommitted changes.

**Sketch MD5** equals the md5sum of `.pio/build/supermini/firmware.bin`, so you
can tell at a glance whether the build you just made is the one running — the
build time cannot do that reliably, because it only changes when the
diagnostics source file is recompiled.

**HTTPS and the heap.** This board has no PSRAM, and the frame sprite
(240×240 at 16 bpp = 115 KB) is held for the life of the process. Every HTTPS
connection needs two contiguous ~16.7 KB mbedTLS buffers
(`MBEDTLS_SSL_MAX_CONTENT_LEN` is 16384 in both directions in the precompiled
framework). When the heap is too fragmented for that, HTTPS calls fail with
`start_ssl_client: -32512` / `SSL - Memory allocation failed` in the serial log
even though the free-heap total still looks comfortable — **largest free block**
and **lowest free ever** are the numbers to watch. On some boots the Wi‑Fi,
mDNS and web-server allocations leave the heap in exactly that state for good.
The firmware reserves 40 KB at the very start of `setup()` and frees it once
everything else is allocated, which makes this rarer, and reboots itself after
three consecutive ADS-B fetches that failed for exactly this reason (about
30 s). The counter lives in RTC memory and stops after five attempts, so a
persistent cause cannot become a reboot loop; **TLS recovery reboots** shows
how often it happened since power-on.

The page streams itself in ~1 KB chunks instead of building the whole document
in RAM, and costs nothing when nobody requests it. Like the rest of the portal
it has **no password**.

## Radar display

### Grid

- Near-black background, green rings and crosshairs
- Tags: route in white, aircraft type in light blue, altitude in orange; runway lines in gold
- Footer (when enabled): dark brown, weather in light blue, time in orange
- White **N / S / E / W** at the bezel; range label on the **east** spoke, drawn just inside the outer ring (the value is the ring‑3 distance = ¾ of outer radius)
- White center dot

Layout and colors: `include/ui/radar_theme.h`. All colors are defined as they
appear on the panel. GC9A01 modules differ in their red/blue order; if yours
shows red aircraft in blue, see **Display colors** under
[Configuration](#configuration).

### Satellite background and rain radar

Both are off by default and switched on under **Setup**.

**Satellite background.** The background becomes a cloud-free Sentinel-2 image
of exactly the visible area. The grid, crosshairs and range label are left out;
aircraft triangles, speed vectors, rim dots, runway lines and the center dot get
a dark outline, and every label sits on a semi-transparent dark backdrop so it
stays readable on any terrain. Each tag is drawn as a unit (backdrop, then
text), so where tags still overlap the later one covers the earlier cleanly.

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
- Runway lines with one ICAO label per airport (e.g. `KJFK`); toggle in the Wi‑Fi setup portal
- The label sits just below the airport's runways (above them for airports north of the center), so it never covers the lines
- Update the embedded list: `python3 scripts/build_large_airports.py`

### Aircraft

- **Up to 110 px from the center** (just inside the panel edge, past the outer ring) — red heading triangle, magenta speed vector (clipped at the panel edge), route / detailed type / altitude tags
- **Beyond that**, up to 1.5× the view radius — small **red dot on the screen rim** at the correct bearing, announcing traffic before it enters the view (direction cue; not distance-accurate)
- **Tags** — placed to avoid each other: by default toward the **center** (west → tag on the **right** of the symbol, east → on the **left**). If that spot is taken by another tag, a symbol, a N/S/E/W letter or the footer, the tag moves up or down, above or below the symbol, to the other side, or further out in dense clusters; a thin grey leader line then links it to its aircraft. Aircraft nearest the center choose first and are drawn on top. Near the edge no corner falls off the round panel
- **Route fallback** — the callsign is shown until route data arrives, and remains the fallback when no route is known
- **Detailed type** — aircraft data is compacted for the display (for example `Boeing 737-800` → `B737-800`)

As range increases (or aircraft approach), targets move inward; a rim dot becomes a full symbol as soon as the aircraft is within 110 px of the center.

Origin/destination is not transmitted in ADS-B messages. The firmware enriches each active callsign through [ADSBDB](https://www.adsbdb.com/), rate-limits lookups, and caches successful results for six hours (misses for ten minutes). It first requests aircraft and route together, then retries the callsign-only endpoint whenever that returned no route. Route databases are based on known/scheduled callsigns, so private, repositioning, diverted, or recently changed flights may still have no route or an imperfect match.

### Weather and time

The bottom overlay uses the radar coordinates. Current conditions come from [Open-Meteo](https://open-meteo.com/) every 15 minutes; its location timezone offset and NTP provide the clock. The two rows use plain-language conditions and label relative humidity (`RH`):

```text
OVERCAST 82F RH100%
21:45 25 JUL
```

The footer and the weather row are off on a freshly flashed device; switch
them on in **Setup**. Radar and footer text defaults to 110% and can be
adjusted from 80–130% in the same page.

### ADS-B

- Source: `https://opendata.adsb.fi/api/v3/`
- Route and aircraft enrichment: `https://api.adsbdb.com/v0/`
- Fetch radius: `ui::radar::fetchRadiusKm()` — 1.5× the view radius (`kRimDotReachFactor` in `config.h`), so rim dots announce aircraft up to half a screen width before they enter the view
- Poll interval: `kAdsbFetchIntervalMs` (3 s) in `config.h`
- The response is parsed one aircraft at a time straight from the TLS stream, keeping only the fields the display uses, so memory use does not grow with traffic
- Ground aircraft hidden by default (`kAdsbShowGroundAircraft`)
- Hard caps: 64 aircraft per fetch (the nearest are kept), 32 airport labels per frame, 48 cached enrichment results
- Wi‑Fi TX power is capped at 8.5 dBm in both AP and STA mode (`wifi_setup.cpp`) to keep weak USB supplies stable — raise it if range matters more than brown-out margin

The device sends the configured coordinates to adsb.fi and Open-Meteo, and sends active callsign/Mode-S identifiers to ADSBDB. With the satellite background or rain overlay enabled, the coordinates also go to EOX (tiles.maps.eox.at) or RainViewer. These calls use HTTPS but **do not verify the server certificate** (`setInsecure()`) — there is no room for a certificate bundle, so the traffic is encrypted but not authenticated.

While the **Setup** page is open, your browser — not the device — additionally loads Leaflet from cdnjs, map tiles from openstreetmap.org, and, if you use the search box, sends that query to Nominatim.

## Configuration

Edit **`include/config.h`** for hardware and behavior (then rebuild, see [path C](#c-build-from-source)):

| Area | Keys / notes |
|------|----------------|
| Portal | `kPortalApName`, `kPortalIp`, `kPortalHostname` / `kPortalHostUrl` (mDNS; needs `-DWM_MDNS` in `platformio.ini`) |
| Wi‑Fi timing | connect attempts, reconnect grace, portal timeout (`0` = no timeout) |
| BOOT | `kBootPin`, `kBootResetHoldMs`, `kBootTapMinMs` |
| Display SPI | pins, `kDisplayInvert`, `kDisplaySpiWriteHz` |
| Display colors | Colors in `radar_theme.h` and `config.h` are written as they should appear. `kDisplaySwapRedBlue` says whether the panel shows red and blue swapped relative to what the firmware sends; `config::panelColor()`, the satellite image and the rain overlay compensate for it. `true` matches the reference module. If red aircraft show up blue (or water looks orange in satellite mode), set it to `false`. Leave `kDisplayRgbOrder` as is |
| Default location | `kDefaultRadarLat`, `kDefaultRadarLon` (until portal overrides) |
| ADS-B | `kAdsbApiBase`, `kAdsbFetchIntervalMs`, `kAdsbShowGroundAircraft`, `kRimDotReachFactor` |
| Flight enrichment | lookup interval, timeout, and cache durations |
| Weather | endpoint, request timeout, and refresh interval |
| Satellite background | `kSatelliteWmsBase` (WMS endpoint and layer), request timeout, retry delay |
| Rain radar | `kRainIndexUrl`, check interval, retry delay, request timeout, `kRainMaxAgeSec` |
| Defaults | initial OTA credentials |

Range presets: `include/ui/radar_range.h` (`kRangePresets`). Portal-setting defaults: `src/services/display_settings.cpp` and `src/ui/radar_range.cpp`.

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
  firmware_version.py      — post-build hook: version from the git tag
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

## Building release images (maintainers)

Day-to-day building and flashing is described in [path C](#c-build-from-source).

- PlatformIO env: **`supermini`**
- Serial: **115200** baud
- USB CDC on boot enabled in `platformio.ini` for the Super Mini

### Web-flashable full image

Single `.bin` for [esptool-js](https://espressif.github.io/esptool-js/) and similar tools (ESP32-C3, 4 MB, flash at **0x0**):

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

The merged image fills the gaps between bootloader, partition table and app with `0xFF`, which wipes the NVS area (Wi‑Fi credentials and settings).

### OTA images

The firmware uses two 1.75 MB application slots. The OTA image is the plain app, `.pio/build/supermini/firmware.bin` (released as `-ota.bin`); upload it as in [path A](#a-update-in-the-browser-ota). Never upload the merged/full image to the OTA form; it contains the bootloader and partition table and is only for flashing at offset `0x0`.

### CI and releases (GitHub Actions)

| Workflow | When | Output |
|----------|------|--------|
| [Build](.github/workflows/build.yml) | Push to `main`/`master`, any PR, manual | Artifact `plane-radar-supermini` (merged + split `.bin` files, ~90 days) |
| [Release](.github/workflows/release.yml) | Git tag `v*` (e.g. `v1.0.0`) | GitHub Release `-full.bin` and `-ota.bin` assets + checksums |

To ship a version users can download, create an annotated tag and push it:

```bash
git tag -a v2.1.0 -m "Plane Radar 2.1.0" -m "- What changed
- What else changed"
git push origin v2.1.0
```

The tag message becomes the release text (followed by GitHub's "Full
Changelog" link), and the version number is baked into the firmware. The
release workflow attaches both images. Use `-full.bin` at offset `0x0` for first install/recovery and `-ota.bin` in the device's authenticated firmware page.

## Dependencies

- [LovyanGFX](https://github.com/lovyan03/LovyanGFX)
- [WiFiManager](https://github.com/tzapu/WiFiManager)
- [ArduinoJson](https://github.com/bblanchon/ArduinoJson)

Runtime data services:

- [adsb.fi](https://opendata.adsb.fi/) — nearby aircraft
- [ADSBDB](https://www.adsbdb.com/) — route and detailed aircraft data
- [Open-Meteo](https://open-meteo.com/) — current weather and local timezone offset
- [EOX Sentinel-2 cloudless](https://s2maps.eu/) — satellite background (optional)
- [RainViewer](https://www.rainviewer.com/) — rain radar overlay (optional)
