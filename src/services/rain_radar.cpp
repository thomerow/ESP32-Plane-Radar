#include "services/rain_radar.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <esp32c3/rom/miniz.h>  // tinfl_decompress lives in the C3's ROM

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "config.h"
#include "services/file_cache.h"
#include "ui/pixel_ops.h"

namespace services::rain {
namespace {

constexpr char kPngPath[] = "/rain.png";
/** Decoded intensity grid; streamed row by row while drawing (no heap). */
constexpr char kGridPath[] = "/rain_grid.bin";
/** Free RainViewer tier caps zoom at 7 (~0.74 km/px at 53° N). */
constexpr int kZoom = 7;
constexpr int kTileSize = 256;
constexpr int kScreen = 240;
/** Intensity grid: 80×80 cells of 3×3 screen px, resampled bilinearly per frame. */
constexpr int kGridN = 80;
constexpr int kCellPx = kScreen / kGridN;
constexpr int kMaxPngWidth = 512;
constexpr size_t kInBufSize = 1024;
constexpr time_t kMinimumValidEpoch = 1609459200;  // 2021-01-01
constexpr double kSameCoordDeg = 0.00001;
constexpr double kDegToRad = 0.017453292519943295;
/** Grid values below this (≈4 % intensity) are not drawn. */
constexpr uint8_t kMinVisible = 10;

PollFn s_poll_fn = nullptr;
ScratchFn s_scratch_fn = nullptr;

// Newest frame announced by the index.
char s_host[64] = {};
char s_path[64] = {};
long s_newest_time = 0;
bool s_index_ok = false;
unsigned long s_last_index_ms = 0;

// Frame cached in LittleFS.
long s_png_time = 0;
double s_png_lat = 999.0;
double s_png_lon = 999.0;
bool s_png_ok = false;
size_t s_png_bytes = 0;

// Intensity grid for one view. The heap cannot spare its 6.4 KB without TLS
// handshakes for the other services starting to fail, so it is built in the
// scratch buffer and kept as a file; only this metadata stays in RAM.
bool s_grid_ok = false;
long s_grid_time = 0;
double s_grid_lat = 999.0;
double s_grid_lon = 999.0;
float s_grid_half_km = 0.0f;
uint32_t s_grid_rain_cells = 0;

unsigned long s_last_failure_ms = 0;
unsigned long s_last_decode_failure_ms = 0;
int s_last_error = 0;
const char* s_last_error_where = "";
uint32_t s_frames_downloaded = 0;
unsigned long s_last_decode_ms = 0;

void poll() {
  if (s_poll_fn != nullptr) {
    s_poll_fn();
  }
}

bool sameCoord(double a, double b) { return fabs(a - b) < kSameCoordDeg; }

bool failNetwork(const char* where, int code) {
  s_last_failure_ms = millis();
  s_last_error = code;
  s_last_error_where = where;
  Serial.printf("rain: %s failed (%d)\n", where, code);
  return false;
}

bool failDecode(const char* where) {
  s_last_decode_failure_ms = millis();
  s_last_error = -1;
  s_last_error_where = where;
  Serial.printf("rain: decode failed: %s\n", where);
  return false;
}

bool fetchIndex() {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, config::kRainIndexUrl)) {
    return failNetwork("index begin", -1);
  }
  http.setConnectTimeout(static_cast<int32_t>(config::kRainRequestTimeoutMs));
  http.setTimeout(static_cast<uint16_t>(config::kRainRequestTimeoutMs));
  poll();
  const int code = http.GET();
  poll();
  if (code != HTTP_CODE_OK) {
    http.end();
    return failNetwork("index", code);
  }
  const String payload = http.getString();  // ~1.5 KB
  http.end();
  poll();

  JsonDocument filter;
  filter["host"] = true;
  filter["radar"]["past"][0]["time"] = true;
  filter["radar"]["past"][0]["path"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, payload, DeserializationOption::Filter(filter))) {
    return failNetwork("index json", -2);
  }
  JsonArray past = doc["radar"]["past"].as<JsonArray>();
  const char* host = doc["host"] | "";
  if (past.isNull() || past.size() == 0 || host[0] == '\0') {
    return failNetwork("index empty", -3);
  }
  JsonObject newest = past[past.size() - 1];
  const char* path = newest["path"] | "";
  if (path[0] == '\0') {
    return failNetwork("index path", -3);
  }
  strncpy(s_host, host, sizeof(s_host) - 1);
  strncpy(s_path, path, sizeof(s_path) - 1);
  s_newest_time = newest["time"] | 0L;
  s_index_ok = true;
  s_last_index_ms = millis();
  return true;
}

bool downloadPng(double lat, double lon) {
  char url[200];
  snprintf(url, sizeof(url), "%s%s/%d/%d/%.4f/%.4f/2/0_0.png", s_host, s_path,
           kTileSize, kZoom, lat, lon);
  size_t bytes = 0;
  const int code = files::download(url, kPngPath, config::kRainRequestTimeoutMs,
                                   s_poll_fn, &bytes);
  if (code != HTTP_CODE_OK) {
    s_png_ok = false;
    return failNetwork("frame", code);
  }
  s_png_ok = true;
  s_png_time = s_newest_time;
  s_png_lat = lat;
  s_png_lon = lon;
  s_png_bytes = bytes;
  ++s_frames_downloaded;
  s_last_failure_ms = 0;
  return true;
}

/** RainViewer "Universal Blue" color → 0..255 rain intensity (drizzle → extreme). */
uint8_t universalBlueIntensity(const uint8_t* px) {
  const int r = px[0];
  const int g = px[1];
  const int b = px[2];
  const int a = px[3];
  if (a == 0) {
    return 0;
  }
  float t = 1.0f;
  if (a < 255) {
    t = std::min(1.0f, a / 190.0f) * 0.30f;  // translucent beige: drizzle
  } else if (b > 90 && r < 160) {
    t = 0.30f + 0.35f * std::min(1.0f, std::max(0.0f, (238 - b) / 134.0f));
  } else if (r > 180 && b < 60) {
    t = 0.65f + 0.30f * std::min(1.0f, std::max(0.0f, (238 - g) / 238.0f));
  }
  return static_cast<uint8_t>(t * 255.0f + 0.5f);
}

uint8_t paeth(int a, int b, int c) {
  const int p = a + b - c;
  const int pa = abs(p - a);
  const int pb = abs(p - b);
  const int pc = abs(p - c);
  if (pa <= pb && pa <= pc) return static_cast<uint8_t>(a);
  if (pb <= pc) return static_cast<uint8_t>(b);
  return static_cast<uint8_t>(c);
}

bool unfilterRow(uint8_t* cur, const uint8_t* prev, size_t n) {
  const uint8_t filter = cur[0];
  uint8_t* c = cur + 1;
  const uint8_t* p = prev + 1;
  constexpr size_t kBpp = 4;
  switch (filter) {
    case 0:
      return true;
    case 1:
      for (size_t k = kBpp; k < n; ++k) c[k] = static_cast<uint8_t>(c[k] + c[k - kBpp]);
      return true;
    case 2:
      for (size_t k = 0; k < n; ++k) c[k] = static_cast<uint8_t>(c[k] + p[k]);
      return true;
    case 3:
      for (size_t k = 0; k < n; ++k) {
        const int left = k >= kBpp ? c[k - kBpp] : 0;
        c[k] = static_cast<uint8_t>(c[k] + ((left + p[k]) >> 1));
      }
      return true;
    case 4:
      for (size_t k = 0; k < n; ++k) {
        const int left = k >= kBpp ? c[k - kBpp] : 0;
        const int up_left = k >= kBpp ? p[k - kBpp] : 0;
        c[k] = static_cast<uint8_t>(c[k] + paeth(left, p[k], up_left));
      }
      return true;
    default:
      return false;
  }
}

uint32_t readU32(File& f) {
  uint8_t b[4] = {};
  if (f.read(b, 4) != 4) {
    return 0xFFFFFFFFu;
  }
  return (static_cast<uint32_t>(b[0]) << 24) | (static_cast<uint32_t>(b[1]) << 16) |
         (static_cast<uint32_t>(b[2]) << 8) | b[3];
}

/** Streams the concatenated IDAT payload of a PNG file. */
class IdatReader {
 public:
  explicit IdatReader(File& f) : f_(f) {}

  /** Parse the header and chunks up to the first IDAT. */
  bool begin(uint32_t* width, uint32_t* height) {
    static const uint8_t kSig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    uint8_t sig[8] = {};
    if (f_.read(sig, 8) != 8 || memcmp(sig, kSig, 8) != 0) {
      return false;
    }
    bool have_header = false;
    for (;;) {
      const uint32_t len = readU32(f_);
      char type[5] = {};
      if (len == 0xFFFFFFFFu || f_.read(reinterpret_cast<uint8_t*>(type), 4) != 4) {
        return false;
      }
      if (strcmp(type, "IHDR") == 0) {
        *width = readU32(f_);
        *height = readU32(f_);
        uint8_t rest[5] = {};
        f_.read(rest, 5);
        // 8-bit RGBA, no interlace: what RainViewer serves.
        if (rest[0] != 8 || rest[1] != 6 || rest[4] != 0) {
          return false;
        }
        f_.seek(4, SeekCur);  // CRC
        have_header = true;
      } else if (strcmp(type, "IDAT") == 0) {
        left_ = len;
        return have_header;
      } else if (strcmp(type, "IEND") == 0) {
        return false;
      } else {
        f_.seek(len + 4, SeekCur);
      }
    }
  }

  size_t read(uint8_t* dst, size_t cap) {
    size_t got = 0;
    while (got < cap && !done_) {
      if (left_ == 0) {
        f_.seek(4, SeekCur);  // CRC of the finished IDAT
        const uint32_t len = readU32(f_);
        char type[5] = {};
        if (len == 0xFFFFFFFFu ||
            f_.read(reinterpret_cast<uint8_t*>(type), 4) != 4 ||
            strcmp(type, "IDAT") != 0) {
          done_ = true;
          break;
        }
        left_ = len;
        continue;
      }
      const size_t want = std::min(cap - got, static_cast<size_t>(left_));
      const size_t n = f_.read(dst + got, want);
      if (n == 0) {
        done_ = true;
        break;
      }
      got += n;
      left_ -= n;
    }
    return got;
  }

 private:
  File& f_;
  uint32_t left_ = 0;
  bool done_ = false;
};

uint8_t* alignUp(uint8_t* p) {
  return reinterpret_cast<uint8_t*>((reinterpret_cast<uintptr_t>(p) + 7u) & ~static_cast<uintptr_t>(7u));
}

bool decodeGrid(double lat, double lon, float half_width_km) {
  size_t scratch_len = 0;
  uint8_t* scratch = s_scratch_fn != nullptr ? s_scratch_fn(&scratch_len) : nullptr;
  if (scratch == nullptr) {
    return failDecode("no scratch buffer");
  }

  File file = LittleFS.open(kPngPath, FILE_READ);
  if (!file) {
    s_png_ok = false;
    return failDecode("png missing");
  }
  const unsigned long started = millis();
  IdatReader idat(file);
  uint32_t w = 0;
  uint32_t h = 0;
  if (!idat.begin(&w, &h) || w == 0 || h == 0 || w > kMaxPngWidth ||
      h > kMaxPngWidth) {
    file.close();
    return failDecode("unsupported png");
  }

  // Carve the scratch buffer: inflator state, 32 KB window, two rows, input.
  const size_t row_bytes = 1 + static_cast<size_t>(w) * 4;
  uint8_t* p = alignUp(scratch);
  auto* inflator = reinterpret_cast<tinfl_decompressor*>(p);
  p = alignUp(p + sizeof(tinfl_decompressor));
  uint8_t* dict = p;
  p += TINFL_LZ_DICT_SIZE;
  uint8_t* row_a = p;
  p += row_bytes;
  uint8_t* row_b = p;
  p += row_bytes;
  uint8_t* in_buf = p;
  p += kInBufSize;
  uint8_t* grid = p;
  p += kGridN * kGridN;
  if (static_cast<size_t>(p - scratch) > scratch_len) {
    file.close();
    return failDecode("scratch too small");
  }

  // Which PNG row/column feeds each grid cell (nearest neighbour; the grid is
  // at least as fine as the source at every range, so nothing is lost).
  const double km_per_rain_px =
      156.543034 * cos(lat * kDegToRad) / static_cast<double>(1 << kZoom);
  const double km_per_screen_px = half_width_km * 2.0 / kScreen;
  const double ratio = km_per_screen_px / km_per_rain_px;
  int16_t col_idx[kGridN];
  int16_t row_idx[kGridN];
  for (int i = 0; i < kGridN; ++i) {
    const double offset = (i * kCellPx + kCellPx * 0.5 - kScreen / 2.0) * ratio;
    const int c = static_cast<int>(floor(w / 2.0 + offset));
    const int r = static_cast<int>(floor(h / 2.0 + offset));  // screen y grows south, like PNG rows
    col_idx[i] = (c >= 0 && c < static_cast<int>(w)) ? c : -1;
    row_idx[i] = (r >= 0 && r < static_cast<int>(h)) ? r : -1;
  }
  memset(grid, 0, kGridN * kGridN);
  memset(row_a, 0, row_bytes);  // "previous row" for the first scanline

  tinfl_init(inflator);
  uint8_t* prev = row_a;
  uint8_t* cur = row_b;
  size_t row_fill = 0;
  uint32_t y = 0;
  size_t dict_ofs = 0;
  size_t in_avail = 0;
  const uint8_t* in_ptr = in_buf;
  bool input_done = false;
  bool ok = true;
  uint32_t rain_cells = 0;

  while (y < h) {
    if (in_avail == 0 && !input_done) {
      in_avail = idat.read(in_buf, kInBufSize);
      in_ptr = in_buf;
      input_done = in_avail == 0;
    }
    size_t in_size = in_avail;
    size_t out_size = TINFL_LZ_DICT_SIZE - dict_ofs;
    const mz_uint32 flags = TINFL_FLAG_PARSE_ZLIB_HEADER |
                            (input_done ? 0 : TINFL_FLAG_HAS_MORE_INPUT);
    const tinfl_status status = tinfl_decompress(
        inflator, in_ptr, &in_size, dict, dict + dict_ofs, &out_size, flags);
    in_ptr += in_size;
    in_avail -= in_size;

    // Feed the new output into the scanline assembler.
    const uint8_t* out = dict + dict_ofs;
    size_t out_left = out_size;
    while (out_left > 0 && y < h) {
      const size_t take = std::min(out_left, row_bytes - row_fill);
      memcpy(cur + row_fill, out, take);
      row_fill += take;
      out += take;
      out_left -= take;
      if (row_fill < row_bytes) {
        break;
      }
      if (!unfilterRow(cur, prev, row_bytes - 1)) {
        ok = false;
        break;
      }
      for (int j = 0; j < kGridN; ++j) {
        if (row_idx[j] != static_cast<int>(y)) {
          continue;
        }
        uint8_t* grid_row = grid + j * kGridN;
        for (int i = 0; i < kGridN; ++i) {
          if (col_idx[i] < 0) {
            continue;
          }
          const uint8_t v = universalBlueIntensity(cur + 1 + col_idx[i] * 4);
          grid_row[i] = v;
          if (v >= kMinVisible) {
            ++rain_cells;
          }
        }
      }
      std::swap(prev, cur);
      row_fill = 0;
      ++y;
    }
    dict_ofs = (dict_ofs + out_size) & (TINFL_LZ_DICT_SIZE - 1);

    if (!ok || status < TINFL_STATUS_DONE) {
      ok = false;
      break;
    }
    if (status == TINFL_STATUS_DONE) {
      break;
    }
    if (status == TINFL_STATUS_NEEDS_MORE_INPUT && input_done) {
      ok = false;  // truncated stream
      break;
    }
  }
  file.close();

  s_grid_ok = false;
  if (!ok || y < h) {
    return failDecode("inflate/unfilter error");
  }
  File out = LittleFS.open(kGridPath, FILE_WRITE);
  if (!out) {
    return failDecode("grid file");
  }
  const size_t stored = out.write(grid, kGridN * kGridN);
  out.close();
  if (stored != static_cast<size_t>(kGridN * kGridN)) {
    return failDecode("grid write");
  }
  s_grid_ok = true;
  s_grid_time = s_png_time;
  s_grid_lat = lat;
  s_grid_lon = lon;
  s_grid_half_km = half_width_km;
  s_grid_rain_cells = rain_cells;
  s_last_decode_ms = millis() - started;
  Serial.printf("rain: frame %ld decoded in %lu ms, %u/%u cells with rain\n",
                s_png_time, s_last_decode_ms, static_cast<unsigned>(rain_cells),
                static_cast<unsigned>(kGridN * kGridN));
  return true;
}

bool gridFits(double lat, double lon, float half_width_km) {
  return s_grid_ok && sameCoord(s_grid_lat, lat) && sameCoord(s_grid_lon, lon) &&
         fabsf(s_grid_half_km - half_width_km) < 0.01f;
}

bool frameStale() {
  const time_t now = time(nullptr);
  return now >= kMinimumValidEpoch && s_grid_time > 0 &&
         now - s_grid_time > config::kRainMaxAgeSec;
}

/** Overlay color ramp: cyan → yellow → magenta, alpha grows with intensity. */
struct RampEntry {
  uint8_t r5;
  uint8_t g6;
  uint8_t b5;
  uint8_t alpha;  // 0..255
};
constexpr int kRampSize = 32;
RampEntry s_ramp[kRampSize];
bool s_ramp_ready = false;

void buildRamp() {
  for (int i = 0; i < kRampSize; ++i) {
    const float t = (i + 0.5f) / kRampSize;
    float r, g, b;
    if (t < 0.5f) {
      const float u = t / 0.5f;
      r = 0 + (255 - 0) * u;
      g = 220 + (230 - 220) * u;
      b = 255 + (0 - 255) * u;
    } else {
      const float u = (t - 0.5f) / 0.5f;
      r = 255;
      g = 230 + (40 - 230) * u;
      b = 0 + (200 - 0) * u;
    }
    if (config::kDisplaySwapRedBlue) {
      std::swap(r, b);
    }
    const float alpha = 0.10f + 0.55f * powf(t, 1.5f);
    s_ramp[i].r5 = static_cast<uint8_t>(static_cast<int>(r) >> 3);
    s_ramp[i].g6 = static_cast<uint8_t>(static_cast<int>(g) >> 2);
    s_ramp[i].b5 = static_cast<uint8_t>(static_cast<int>(b) >> 3);
    s_ramp[i].alpha = static_cast<uint8_t>(alpha * 255.0f);
  }
  s_ramp_ready = true;
}

/** Per-axis bilinear taps from screen px to grid cells (same for x and y). */
struct Tap {
  uint8_t i0;
  uint8_t i1;
  uint8_t w;  // weight of i1, 0..255
};

void buildTaps(Tap* taps) {
  for (int x = 0; x < kScreen; ++x) {
    const float pos = (x + 0.5f) / kCellPx - 0.5f;
    int i0 = static_cast<int>(floorf(pos));
    float frac = pos - i0;
    if (i0 < 0) {
      i0 = 0;
      frac = 0.0f;
    }
    int i1 = i0 + 1;
    if (i1 >= kGridN) {
      i1 = kGridN - 1;
    }
    if (i0 >= kGridN) {
      i0 = kGridN - 1;
    }
    taps[x].i0 = static_cast<uint8_t>(i0);
    taps[x].i1 = static_cast<uint8_t>(i1);
    taps[x].w = static_cast<uint8_t>(frac * 255.0f);
  }
}

}  // namespace

void setPollFn(PollFn fn) { s_poll_fn = fn; }

void setScratchProvider(ScratchFn fn) { s_scratch_fn = fn; }

bool refreshIfDue(double lat, double lon, float half_width_km) {
  const unsigned long now = millis();
  const bool network_backoff =
      s_last_failure_ms != 0 && now - s_last_failure_ms < config::kRainRetryMs;
  bool png_here = s_png_ok && sameCoord(s_png_lat, lat) && sameCoord(s_png_lon, lon);
  const bool index_due =
      !s_index_ok || now - s_last_index_ms >= config::kRainCheckIntervalMs;

  if (!network_backoff && (index_due || !png_here)) {
    bool index_ready = s_index_ok && !index_due;
    if (!index_ready) {
      index_ready = fetchIndex();
    }
    if (index_ready && (!png_here || s_png_time != s_newest_time)) {
      png_here = downloadPng(lat, lon);
    }
  }
  if (!png_here) {
    return false;
  }
  if (gridFits(lat, lon, half_width_km) && s_grid_time == s_png_time) {
    return false;
  }
  if (s_last_decode_failure_ms != 0 &&
      now - s_last_decode_failure_ms < config::kRainRetryMs) {
    return false;
  }
  if (!files::mount()) {
    return false;
  }
  if (decodeGrid(lat, lon, half_width_km)) {
    s_last_decode_failure_ms = 0;
  }
  // The scratch (frame sprite) was clobbered either way; ask for a redraw.
  return true;
}

void drawOverlay(LGFX_Sprite& sprite, double lat, double lon,
                 float half_width_km) {
  if (!gridFits(lat, lon, half_width_km) || frameStale() ||
      s_grid_rain_cells == 0) {
    return;
  }
  auto* buf = static_cast<uint16_t*>(sprite.getBuffer());
  if (buf == nullptr || sprite.getColorDepth() != 16 ||
      sprite.width() != kScreen || sprite.height() != kScreen) {
    return;
  }
  if (!s_ramp_ready) {
    buildRamp();
  }
  File file = LittleFS.open(kGridPath, FILE_READ);
  if (!file) {
    s_grid_ok = false;
    return;
  }
  Tap taps[kScreen];  // 720 B on the loop stack, only while drawing
  buildTaps(taps);

  // Two-row cache: the screen walks the grid top to bottom, so every grid row
  // is read from flash exactly once per frame.
  uint8_t rows[2][kGridN];
  int row_ids[2] = {-1, -1};
  auto gridRow = [&](int idx) -> const uint8_t* {
    for (int k = 0; k < 2; ++k) {
      if (row_ids[k] == idx) {
        return rows[k];
      }
    }
    const int slot = row_ids[0] < row_ids[1] ? 0 : 1;  // drop the older row
    file.seek(static_cast<uint32_t>(idx) * kGridN, SeekSet);
    if (file.read(rows[slot], kGridN) != kGridN) {
      memset(rows[slot], 0, kGridN);
    }
    row_ids[slot] = idx;
    return rows[slot];
  };

  for (int y = 0; y < kScreen; ++y) {
    const Tap& ty = taps[y];
    const uint8_t* row0 = gridRow(ty.i0);
    const uint8_t* row1 = gridRow(ty.i1);
    const uint32_t wy = ty.w;
    uint16_t* out = buf + y * kScreen;
    for (int x = 0; x < kScreen; ++x) {
      const Tap& tx = taps[x];
      const uint32_t a = row0[tx.i0];
      const uint32_t b = row0[tx.i1];
      const uint32_t c = row1[tx.i0];
      const uint32_t d = row1[tx.i1];
      if ((a | b | c | d) == 0) {
        continue;
      }
      const uint32_t wx = tx.w;
      const uint32_t top = a * (255 - wx) + b * wx;
      const uint32_t bottom = c * (255 - wx) + d * wx;
      const uint32_t t = (top * (255 - wy) + bottom * wy) / (255U * 255U);
      if (t < kMinVisible) {
        continue;
      }
      const RampEntry& e = s_ramp[t * kRampSize / 256];
      const uint32_t al = e.alpha;
      const uint32_t inv = 255 - al;
      const uint16_t v = ui::pixels::unswap(out[x]);
      const uint32_t r = (((v >> 11) & 0x1F) * inv + e.r5 * al) / 255;
      const uint32_t g = (((v >> 5) & 0x3F) * inv + e.g6 * al) / 255;
      const uint32_t bl = ((v & 0x1F) * inv + e.b5 * al) / 255;
      out[x] = ui::pixels::unswap(static_cast<uint16_t>((r << 11) | (g << 5) | bl));
    }
  }
  file.close();
}

void diagnostics(Print& out) {
  char buf[240];
  const time_t now = time(nullptr);
  long age_min = -1;
  if (now >= kMinimumValidEpoch && s_grid_time > 0) {
    age_min = static_cast<long>((now - s_grid_time) / 60);
  }
  snprintf(buf, sizeof(buf),
           "<tr><th>Rain radar</th><td>%s, frame age %ld min, %u of %u cells "
           "with rain, %lu frames downloaded, PNG %u B, decode %lu ms</td></tr>",
           s_grid_ok ? (frameStale() ? "stale" : "ready") : "no frame yet",
           age_min, static_cast<unsigned>(s_grid_rain_cells),
           static_cast<unsigned>(kGridN * kGridN),
           static_cast<unsigned long>(s_frames_downloaded),
           static_cast<unsigned>(s_png_bytes),
           static_cast<unsigned long>(s_last_decode_ms));
  out.print(buf);
  if (s_last_error != 0) {
    snprintf(buf, sizeof(buf),
             "<tr><th>Rain last error</th><td>%s (%d)</td></tr>",
             s_last_error_where, s_last_error);
    out.print(buf);
  }
}

}  // namespace services::rain
