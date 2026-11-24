#include "services/satellite_map.h"

#include <cmath>
#include <cstdio>

#include "config.h"
#include "services/file_cache.h"
#include "ui/pixel_ops.h"

namespace services::satmap {
namespace {

constexpr int kImageSize = 240;
constexpr double kKmPerDeg = 111.0;
constexpr double kDegToRad = 0.017453292519943295;
/** Location changes below this (≈1 m) do not invalidate a cached image. */
constexpr double kSameCoordDeg = 0.00001;

PollFn s_poll_fn = nullptr;
unsigned long s_last_failure_ms = 0;
int s_last_error = 0;
uint32_t s_downloads = 0;
unsigned long s_last_decode_ms = 0;

/** What we last learned about one cache file, so frames don't re-read metadata. */
struct Known {
  int key = -1;
  double lat = 999.0;
  double lon = 999.0;
  bool present = false;
};
Known s_known;

/** Files are keyed by view width (one per range preset), metadata holds lat/lon. */
int viewKey(float half_width_km) {
  return static_cast<int>(lroundf(half_width_km * 10.0f));
}

void imagePath(int key, char* out, size_t len) {
  snprintf(out, len, "/sat_%d.jpg", key);
}

void metaPath(int key, char* out, size_t len) {
  snprintf(out, len, "/sat_%d.txt", key);
}

bool sameCoord(double a, double b) { return fabs(a - b) < kSameCoordDeg; }

/** LovyanGFX image decoders read through this; wraps an open LittleFS file. */
struct FileWrapper : public lgfx::DataWrapper {
  explicit FileWrapper(File& file) : file_(file) {}
  int read(uint8_t* buf, uint32_t len) override {
    return static_cast<int>(file_.read(buf, len));
  }
  void skip(int32_t offset) override { file_.seek(offset, SeekCur); }
  bool seek(uint32_t offset) override { return file_.seek(offset, SeekSet); }
  void close() override {}
  int32_t tell() override { return static_cast<int32_t>(file_.position()); }

 private:
  File& file_;
};

void loadKnown(int key) {
  s_known = Known();
  s_known.key = key;
  if (!files::mount()) {
    return;
  }
  char path[24];
  imagePath(key, path, sizeof(path));
  if (!LittleFS.exists(path)) {
    return;
  }
  metaPath(key, path, sizeof(path));
  File meta = LittleFS.open(path, FILE_READ);
  if (!meta) {
    return;
  }
  char text[64] = {};
  meta.readBytes(text, sizeof(text) - 1);
  meta.close();
  double lat = 0.0;
  double lon = 0.0;
  if (sscanf(text, "%lf %lf", &lat, &lon) == 2) {
    s_known.lat = lat;
    s_known.lon = lon;
    s_known.present = true;
  }
}

}  // namespace

void setPollFn(PollFn fn) { s_poll_fn = fn; }

bool hasImage(double lat, double lon, float half_width_km) {
  const int key = viewKey(half_width_km);
  if (s_known.key != key) {
    loadKnown(key);
  }
  return s_known.present && sameCoord(s_known.lat, lat) &&
         sameCoord(s_known.lon, lon);
}

bool refreshIfNeeded(double lat, double lon, float half_width_km) {
  if (hasImage(lat, lon, half_width_km)) {
    return false;
  }
  if (s_last_failure_ms != 0 &&
      millis() - s_last_failure_ms < config::kSatelliteRetryMs) {
    return false;
  }

  const double dlat = half_width_km / kKmPerDeg;
  const double dlon = half_width_km / (kKmPerDeg * cos(lat * kDegToRad));
  char url[320];
  snprintf(url, sizeof(url), "%s&BBOX=%.5f,%.5f,%.5f,%.5f&WIDTH=%d&HEIGHT=%d",
           config::kSatelliteWmsBase, lon - dlon, lat - dlat, lon + dlon,
           lat + dlat, kImageSize, kImageSize);

  const int key = viewKey(half_width_km);
  char path[24];
  imagePath(key, path, sizeof(path));
  size_t bytes = 0;
  const int code = files::download(url, path, config::kSatelliteRequestTimeoutMs,
                                   s_poll_fn, &bytes);
  if (code != 200) {
    s_last_failure_ms = millis();
    s_last_error = code;
    Serial.printf("satellite: download failed (%d)\n", code);
    return false;
  }

  char meta_path[24];
  metaPath(key, meta_path, sizeof(meta_path));
  File meta = LittleFS.open(meta_path, FILE_WRITE);
  if (meta) {
    meta.printf("%.6f %.6f\n", lat, lon);
    meta.close();
  }
  s_last_failure_ms = 0;
  s_last_error = 0;
  ++s_downloads;
  s_known.key = key;
  s_known.lat = lat;
  s_known.lon = lon;
  s_known.present = true;
  Serial.printf("satellite: stored %s (%u bytes)\n", path,
                static_cast<unsigned>(bytes));
  return true;
}

bool drawBackground(LGFX_Sprite& sprite, double lat, double lon,
                    float half_width_km, int brightness_pct) {
  if (!hasImage(lat, lon, half_width_km)) {
    return false;
  }
  auto* buf = static_cast<uint16_t*>(sprite.getBuffer());
  if (buf == nullptr || sprite.getColorDepth() != 16 ||
      sprite.width() != kImageSize || sprite.height() != kImageSize) {
    return false;
  }

  const unsigned long started = millis();
  char path[24];
  imagePath(viewKey(half_width_km), path, sizeof(path));
  File file = LittleFS.open(path, FILE_READ);
  if (!file) {
    s_known.present = false;
    return false;
  }
  FileWrapper wrapper(file);
  const bool ok = sprite.drawJpg(&wrapper, 0, 0);
  file.close();
  if (!ok) {
    // A corrupt file would otherwise fail on every frame; drop it and refetch.
    Serial.println("satellite: JPEG decode failed, discarding cache file");
    LittleFS.remove(path);
    s_known.present = false;
    return false;
  }

  // Dim (and, on this panel, swap red/blue) in one pass over the frame.
  const int pct = brightness_pct < 5 ? 5 : (brightness_pct > 100 ? 100 : brightness_pct);
  const uint32_t scale = static_cast<uint32_t>(pct) * 256U / 100U;
  const size_t count = static_cast<size_t>(kImageSize) * kImageSize;
  for (size_t i = 0; i < count; ++i) {
    const uint16_t v = ui::pixels::unswap(buf[i]);
    uint32_t r = (v >> 11) & 0x1F;
    const uint32_t g = ((v >> 5) & 0x3F) * scale >> 8;
    uint32_t b = v & 0x1F;
    if (config::kDisplaySwapRedBlue) {
      const uint32_t t = r;
      r = b;
      b = t;
    }
    r = r * scale >> 8;
    b = b * scale >> 8;
    buf[i] = ui::pixels::unswap(static_cast<uint16_t>((r << 11) | (g << 5) | b));
  }
  s_last_decode_ms = millis() - started;
  return true;
}

void diagnostics(Print& out, double lat, double lon, float half_width_km) {
  char buf[200];
  const bool cached = hasImage(lat, lon, half_width_km);
  snprintf(buf, sizeof(buf),
           "<tr><th>Satellite image</th><td>%s (view %.1f km wide), "
           "%lu downloads, last decode %lu ms</td></tr>",
           cached ? "cached" : "not cached yet",
           static_cast<double>(half_width_km * 2.0f),
           static_cast<unsigned long>(s_downloads),
           static_cast<unsigned long>(s_last_decode_ms));
  out.print(buf);
  if (s_last_error != 0) {
    snprintf(buf, sizeof(buf),
             "<tr><th>Satellite last error</th><td>%d</td></tr>", s_last_error);
    out.print(buf);
  }
}

}  // namespace services::satmap
