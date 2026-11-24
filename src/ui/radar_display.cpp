#include "ui/radar_display.h"

#include <lgfx/v1/lgfx_fonts.hpp>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "config.h"
#include "hardware/display.h"
#include "hardware/display_font.h"
#include "services/adsb_client.h"
#include "services/display_settings.h"
#include "services/radar_location.h"
#include "services/rain_radar.h"
#include "services/satellite_map.h"
#include "services/weather_time.h"
#include "ui/pixel_ops.h"
#include "ui/radar_range.h"
#include "ui/radar_theme.h"
#include "ui/runway_overlay.h"

namespace lgfx_fonts = lgfx::v1::fonts;

namespace ui {
namespace radar {

uint16_t kColorBackground = 0x0000;
uint16_t kColorGrid = 0x0320;
uint16_t kColorLabel = 0xFFFF;
uint16_t kColorCenter = 0xFFFF;
uint16_t kColorAircraft = 0x001F;
uint16_t kColorTrackVector = 0xFFFF;
uint16_t kColorTagType = 0x5DFF;
uint16_t kColorTagAltitude = 0xFFE0;
uint16_t kColorRunway = 0x4D5F;
uint16_t kColorRunwayLabel = 0x7DFF;
uint16_t kColorFooterBackground = 0x0084;

}  // namespace radar

namespace {

bool s_label_metrics_ready = false;
bool s_cardinal_use_vlw = false;
bool s_scale_use_vlw = false;
float s_cardinal_vlw_size = 0.56f;
float s_scale_vlw_size = 0.50f;
float s_tag_vlw_size = 0.56f;
const lgfx::GFXfont* s_cardinal_gfx = &lgfx_fonts::FreeSansBold12pt7b;
const lgfx::GFXfont* s_scale_gfx = &lgfx_fonts::FreeSansBold9pt7b;
const lgfx::GFXfont* s_tag_gfx = &lgfx_fonts::FreeSansBold12pt7b;

bool s_tag_label_metrics_ready = false;
bool s_tag_use_vlw = false;
bool s_footer_metrics_ready = false;
bool s_footer_use_vlw = false;
float s_footer_vlw_size = 0.36f;

lgfx::LovyanGFX* s_draw = &tft;
LGFX_Sprite s_frame(&tft);
bool s_frame_ready = false;
/** True while composing a frame on the satellite image (no grid, outlined symbols). */
bool s_sat_frame = false;

/** Backdrops behind labels keep this share of the image brightness (~30 %). */
constexpr uint16_t kBackdropScale = 77;
constexpr uint16_t kOutlineColor = 0x0000;
/** Mid grey (identical with or without the panel's red/blue swap). */
constexpr uint16_t kColorTagLeader = 0x9CF3;

/** Satellite frames draw text transparently so the image shows around glyphs. */
void setLabelColor(uint16_t fg) {
  if (s_sat_frame) {
    s_draw->setTextColor(fg);
  } else {
    s_draw->setTextColor(fg, radar::kColorBackground);
  }
}

/** Darken a rectangle of the frame behind a label (satellite frames only). */
void backdrop(int x, int y, int w, int h) {
  if (s_sat_frame && s_draw == &s_frame) {
    ui::pixels::darkenRect(s_frame, x, y, w, h, kBackdropScale);
  }
}

class DrawScope {
 public:
  explicit DrawScope(lgfx::LovyanGFX& gfx) : prev_(s_draw) { s_draw = &gfx; }
  ~DrawScope() { s_draw = prev_; }

 private:
  lgfx::LovyanGFX* prev_;
};

float configuredTextScale() {
  return static_cast<float>(services::settings::textScalePercent()) / 100.0f;
}

void applyBitmapTextScale(lgfx::LGFXBase& gfx) {
  gfx.setTextSize(configuredTextScale());
}

int absDiff(int a, int b) { return std::abs(a - b); }

int measureGfxHeight(const lgfx::GFXfont& font) {
  tft.setFont(&font);
  tft.setTextSize(1);
  return tft.fontHeight();
}

int measureVlwHeight(float size) {
  tft.setTextSize(size);
  return tft.fontHeight();
}

float findVlwSizeForHeight(int target_px) {
  float lo = 0.25f;
  float hi = 1.2f;
  for (int i = 0; i < 16; ++i) {
    const float mid = (lo + hi) * 0.5f;
    if (measureVlwHeight(mid) < target_px) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  return hi;
}

void applyScaleStyle();

const lgfx::GFXfont* pickGfxFontClosest(
    int target_px, const lgfx::GFXfont* const* candidates, size_t count) {
  const lgfx::GFXfont* best = candidates[0];
  int best_diff = absDiff(measureGfxHeight(*best), target_px);

  for (size_t i = 1; i < count; ++i) {
    const int diff = absDiff(measureGfxHeight(*candidates[i]), target_px);
    if (diff < best_diff) {
      best_diff = diff;
      best = candidates[i];
    }
  }
  return best;
}

void initLabelMetrics() {
  if (s_label_metrics_ready) {
    return;
  }

  const int cardinal_target = radar::kCardinalLabelHeightPx;

  if (displayFontIsSmooth()) {
    s_cardinal_use_vlw = true;
    s_cardinal_vlw_size = findVlwSizeForHeight(cardinal_target);
    const int cardinal_h = measureVlwHeight(s_cardinal_vlw_size);
    const int scale_target = cardinal_h - radar::kScaleBelowCardinalPx;
    s_scale_use_vlw = true;
    s_scale_vlw_size = findVlwSizeForHeight(scale_target);
  } else {
    const lgfx::GFXfont* cardinal_candidates[] = {
        &lgfx_fonts::FreeSansBold12pt7b, &lgfx_fonts::FreeSansBold9pt7b};
    s_cardinal_gfx =
        pickGfxFontClosest(cardinal_target, cardinal_candidates, 2);
    s_cardinal_use_vlw = false;

    const int cardinal_h = measureGfxHeight(*s_cardinal_gfx);
    const int scale_target = cardinal_h - radar::kScaleBelowCardinalPx;
    const lgfx::GFXfont* scale_candidates[] = {
        &lgfx_fonts::FreeSansBold9pt7b, &lgfx_fonts::FreeSansBold12pt7b};
    s_scale_gfx = pickGfxFontClosest(scale_target, scale_candidates, 2);
    s_scale_use_vlw = false;
  }

  applyScaleStyle();
  s_label_metrics_ready = true;
}

void initTagLabelMetrics() {
  if (s_tag_label_metrics_ready) {
    return;
  }

  const int target = radar::kAircraftTagLabelHeightPx;
  if (displayFontIsSmooth()) {
    s_tag_use_vlw = true;
    s_tag_vlw_size = findVlwSizeForHeight(target);
  } else {
    const lgfx::GFXfont* tag_candidates[] = {
        &lgfx_fonts::FreeSansBold12pt7b, &lgfx_fonts::FreeSansBold9pt7b};
    s_tag_gfx = pickGfxFontClosest(target, tag_candidates, 2);
    s_tag_use_vlw = false;
  }

  s_tag_label_metrics_ready = true;
}

void initFooterMetrics() {
  if (s_footer_metrics_ready) {
    return;
  }
  if (displayFontIsSmooth()) {
    s_footer_use_vlw = true;
    s_footer_vlw_size =
        findVlwSizeForHeight(radar::kFooterLabelHeightPx);
  }
  s_footer_metrics_ready = true;
}

void initPalette() {
  radar::kColorBackground = tft.color565(radar::kBgR, radar::kBgG, radar::kBgB);
  radar::kColorGrid = tft.color565(radar::kGridR, radar::kGridG, radar::kGridB);
  radar::kColorLabel = tft.color565(255, 255, 255);
  radar::kColorCenter = tft.color565(255, 255, 255);
  // GC9A01 BGR panel: swap R/B in color565 so logical red renders red on screen.
  if (config::kDisplayRgbOrder) {
    radar::kColorAircraft =
        tft.color565(radar::kAircraftB, radar::kAircraftG, radar::kAircraftR);
  } else {
    radar::kColorAircraft =
        tft.color565(radar::kAircraftR, radar::kAircraftG, radar::kAircraftB);
  }
  radar::kColorTrackVector =
      tft.color565(radar::kTrackR, radar::kTrackG, radar::kTrackB);
  radar::kColorTagType =
      tft.color565(radar::kTagTypeR, radar::kTagTypeG, radar::kTagTypeB);
  radar::kColorTagAltitude =
      tft.color565(radar::kTagAltR, radar::kTagAltG, radar::kTagAltB);
  radar::kColorRunway =
      tft.color565(radar::kRunwayR, radar::kRunwayG, radar::kRunwayB);
  radar::kColorRunwayLabel = tft.color565(radar::kRunwayLabelR, radar::kRunwayLabelG,
                                          radar::kRunwayLabelB);
  radar::kColorFooterBackground =
      tft.color565(radar::kFooterBgR, radar::kFooterBgG, radar::kFooterBgB);
}

constexpr float kKmPerDeg = 111.0f;
constexpr float kDegToRad = 0.0174532925f;

/**
 * East-west scale at the radar center: a degree of longitude is 111 km only at
 * the equator and shrinks with cos(latitude) — 67 km at 53°N. Cached because it
 * only changes when the configured position does.
 */
float lonScale() {
  static float cached_lat = 1000.0f;  // outside valid latitudes, forces a fill
  static float cached_scale = 1.0f;
  const float lat = static_cast<float>(services::location::lat());
  if (lat != cached_lat) {
    cached_lat = lat;
    cached_scale = cosf(lat * kDegToRad);
  }
  return cached_scale;
}

void offsetKmFromCenter(float lat, float lon, float* dx_km, float* dy_km,
                        float* dist_km) {
  *dx_km = static_cast<float>(lon - services::location::lon()) * kKmPerDeg *
           lonScale();
  *dy_km =
      static_cast<float>(lat - services::location::lat()) * kKmPerDeg;
  *dist_km = sqrtf((*dx_km) * (*dx_km) + (*dy_km) * (*dy_km));
}

/** Distance (km) up to which aircraft are drawn as symbols, not rim dots. */
float symbolMaxKm() {
  const float outer_km = radar::rangeCurrent().outer_km;
  return outer_km * (static_cast<float>(radar::kAircraftMaxRadiusPx) /
                     static_cast<float>(radar::kGridOuterRadius));
}

/** Flat lat/lon as x/y: 1° ≈ 111 km, north = screen up. */
void latLonToScreen(float lat, float lon, int* out_x, int* out_y) {
  const float outer_km = radar::rangeCurrent().outer_km;
  const float px_per_km = static_cast<float>(radar::kGridOuterRadius) / outer_km;

  float dx_km = 0.0f;
  float dy_km = 0.0f;
  float dist_km = 0.0f;
  offsetKmFromCenter(lat, lon, &dx_km, &dy_km, &dist_km);

  *out_x = radar::kCenterX + static_cast<int>(lroundf(dx_km * px_per_km));
  *out_y = radar::kCenterY - static_cast<int>(lroundf(dy_km * px_per_km));
}

bool isSymbolDistanceKm(float dist_km) { return dist_km <= symbolMaxKm(); }

int distSqFromCenter(int x, int y) {
  const int dx = x - radar::kCenterX;
  const int dy = y - radar::kCenterY;
  return dx * dx + dy * dy;
}

/** Rim dot from true bearing; always on screen edge (even if target is 50+ km away). */
bool beyondRingEdgeDotFromLatLon(float lat, float lon, int* out_x, int* out_y) {
  float dx_km = 0.0f;
  float dy_km = 0.0f;
  float dist_km = 0.0f;
  offsetKmFromCenter(lat, lon, &dx_km, &dy_km, &dist_km);
  if (dist_km < 0.01f) {
    return false;
  }
  if (isSymbolDistanceKm(dist_km)) {
    return false;
  }

  const int cx = radar::kCenterX;
  const int cy = radar::kCenterY;
  const int rim_r = radar::kCenterX - radar::kBeyondRingScreenMarginPx;
  const float angle_rad = atan2f(dx_km, dy_km);

  *out_x = cx + static_cast<int>(lroundf(sinf(angle_rad) * rim_r));
  *out_y = cy - static_cast<int>(lroundf(cosf(angle_rad) * rim_r));
  return true;
}

void drawBeyondRingDot(int x, int y) {
  if (s_sat_frame) {
    s_draw->fillSmoothCircle(x, y, radar::kBeyondRingDotRadiusPx + 1,
                             kOutlineColor);
  }
  s_draw->fillSmoothCircle(x, y, radar::kBeyondRingDotRadiusPx,
                           radar::kColorAircraft);
}

/** Shorten the segment (x0,y0)→(x1,y1) so its end stays on the round panel. */
void clipPointToScreenEdge(int x0, int y0, int* x1, int* y1) {
  const int max_r = radar::kScreenRadiusPx;
  const int max_r_sq = max_r * max_r;
  if (distSqFromCenter(*x1, *y1) <= max_r_sq) {
    return;
  }

  const int dx = *x1 - x0;
  const int dy = *y1 - y0;
  float t = 1.0f;
  for (int step = 0; step < 20; ++step) {
    const int px = x0 + static_cast<int>(lroundf(dx * t));
    const int py = y0 + static_cast<int>(lroundf(dy * t));
    if (distSqFromCenter(px, py) <= max_r_sq) {
      *x1 = px;
      *y1 = py;
      return;
    }
    t -= 0.05f;
    if (t <= 0.0f) {
      *x1 = x0;
      *y1 = y0;
      return;
    }
  }
}

int speedLineLengthPx(float gs_knots) {
  if (gs_knots <= 0.0f) {
    return 0;
  }

  // Fixed screen scale: 60 s horizon at gs, not tied to current range zoom.
  constexpr float kKmPerKnotPerHorizon =
      1.852f * radar::kAircraftTrackHorizonSec / 3600.0f;
  const float px =
      gs_knots * kKmPerKnotPerHorizon * radar::kGridOuterRadius /
      radar::kAircraftTrackRefOuterKm * radar::kAircraftTrackLengthScale;

  const int len = static_cast<int>(px + 0.5f);
  if (len < radar::kAircraftSpeedLineMinPx) {
    return radar::kAircraftSpeedLineMinPx;
  }
  return len;
}

void noseTip(int cx, int cy, float heading_deg, int* tip_x, int* tip_y) {
  constexpr float kDegToRad = 0.01745329252f;
  const float rad = heading_deg * kDegToRad;
  *tip_x = cx + static_cast<int>(lroundf(sinf(rad) * radar::kAircraftNoseLenPx));
  *tip_y = cy - static_cast<int>(lroundf(cosf(rad) * radar::kAircraftNoseLenPx));
}

/** grow_px > 0 enlarges the triangle on all sides (used for the dark outline). */
void drawHeadingTriangle(int cx, int cy, float heading_deg, uint16_t color,
                         float grow_px = 0.0f) {
  constexpr float kDegToRad = 0.01745329252f;
  const float rad = heading_deg * kDegToRad;
  const float sin_h = sinf(rad);
  const float cos_h = cosf(rad);

  const float nose = radar::kAircraftNoseLenPx + grow_px * 2.0f;
  const int tip_x = cx + static_cast<int>(lroundf(sin_h * nose));
  const int tip_y = cy - static_cast<int>(lroundf(cos_h * nose));

  const float tail = radar::kAircraftTailLenPx + grow_px;
  const int base_x = cx - static_cast<int>(lroundf(sin_h * tail));
  const int base_y = cy + static_cast<int>(lroundf(cos_h * tail));

  const float half = radar::kAircraftTailHalfPx + grow_px * 1.5f;
  const int wing_x = static_cast<int>(lroundf(cos_h * half));
  const int wing_y = static_cast<int>(lroundf(sin_h * half));

  s_draw->fillTriangle(tip_x, tip_y, base_x + wing_x, base_y + wing_y,
                       base_x - wing_x, base_y - wing_y, color);
}

void drawSpeedVector(int cx, int cy, float heading_deg, float track_deg,
                     float gs_knots, uint16_t color) {
  const int len = speedLineLengthPx(gs_knots);
  if (len <= 0) {
    return;
  }

  int tip_x = 0;
  int tip_y = 0;
  noseTip(cx, cy, heading_deg, &tip_x, &tip_y);

  constexpr float kDegToRad = 0.01745329252f;
  const float rad = track_deg * kDegToRad;
  int ex = tip_x + static_cast<int>(lroundf(sinf(rad) * len));
  int ey = tip_y - static_cast<int>(lroundf(cosf(rad) * len));
  clipPointToScreenEdge(tip_x, tip_y, &ex, &ey);
  if (ex == tip_x && ey == tip_y) {
    return;
  }
  if (s_sat_frame) {
    s_draw->drawWideLine(tip_x, tip_y, ex, ey,
                         radar::kAircraftTrackLineHalfWidth + 1.0f,
                         kOutlineColor);
  }
  s_draw->drawWideLine(tip_x, tip_y, ex, ey, radar::kAircraftTrackLineHalfWidth,
                       color);
}

void applyTagStyle() {
  if (s_tag_use_vlw) {
    displayFontSetSmoothSize(*s_draw,
                             s_tag_vlw_size * configuredTextScale());
  } else {
    displayFontSetBitmap(*s_draw, s_tag_gfx);
    applyBitmapTextScale(*s_draw);
  }
}

int measureTagBlockWidth(const services::adsb::Aircraft& plane) {
  applyTagStyle();
  int max_w = 0;
  const char* identity =
      plane.route[0] != '\0' ? plane.route : plane.callsign;
  if (identity[0] != '\0') {
    const int w = s_draw->textWidth(identity);
    if (w > max_w) {
      max_w = w;
    }
  }
  if (plane.type[0] != '\0') {
    const int w = s_draw->textWidth(plane.type);
    if (w > max_w) {
      max_w = w;
    }
  }
  if (plane.alt[0] != '\0') {
    const int w = s_draw->textWidth(plane.alt);
    if (w > max_w) {
      max_w = w;
    }
  }
  return max_w;
}

struct TagLayout {
  int anchor_x = 0;
  int top_y = 0;
  int block_w = 0;
  int block_h = 0;
  int line_h = 0;
  bool on_right = true;
};

/** Block size of one aircraft's three-line tag in the current tag style. */
TagLayout measureAircraftTag(const services::adsb::Aircraft& plane) {
  initTagLabelMetrics();
  applyTagStyle();
  TagLayout layout;
  layout.line_h = s_draw->fontHeight();
  layout.block_w = measureTagBlockWidth(plane);
  layout.block_h = layout.line_h * 3;
  return layout;
}

/** Compact screen rectangle (half-open) for tag placement; lives on the stack. */
struct Box {
  int16_t x0;
  int16_t y0;
  int16_t x1;
  int16_t y1;
};

Box makeBox(int x0, int y0, int x1, int y1) {
  return Box{static_cast<int16_t>(x0), static_cast<int16_t>(y0),
             static_cast<int16_t>(x1), static_cast<int16_t>(y1)};
}

int overlapArea(const Box& a, const Box& b) {
  const int w = std::min(a.x1, b.x1) - std::max(a.x0, b.x0);
  const int h = std::min(a.y1, b.y1) - std::max(a.y0, b.y0);
  return (w > 0 && h > 0) ? w * h : 0;
}

/** The area a tag occupies, including its backdrop padding. */
Box tagBox(const TagLayout& layout) {
  const int left =
      layout.on_right ? layout.anchor_x : layout.anchor_x - layout.block_w;
  return makeBox(left - 2, layout.top_y - 1, left + layout.block_w + 2,
                 layout.top_y + layout.block_h + 1);
}

/** Box around an aircraft symbol (nose length plus outline on every side). */
Box symbolBox(int x, int y) {
  constexpr int kHalf = radar::kAircraftNoseLenPx + 2;
  return makeBox(x - kHalf, y - kHalf, x + kHalf + 1, y + kHalf + 1);
}

/** How far (px, summed over corners) a box reaches past the round panel. */
int offPanelPx(const Box& b) {
  const float max_r = static_cast<float>(radar::kScreenRadiusPx - 1);
  const int xs[2] = {b.x0, b.x1};
  const int ys[2] = {b.y0, b.y1};
  int over = 0;
  for (int xi = 0; xi < 2; ++xi) {
    for (int yi = 0; yi < 2; ++yi) {
      const float d = sqrtf(static_cast<float>(distSqFromCenter(xs[xi], ys[yi])));
      if (d > max_r) {
        over += static_cast<int>(d - max_r + 0.5f);
      }
    }
  }
  return over;
}

/**
 * Tag block with its top-left corner at (left, top), clamped to the screen.
 * text_left_aligned: text hugs the left edge (tag right of / above / below the
 * symbol) or the right edge (tag left of the symbol).
 */
TagLayout tagAt(TagLayout layout, int left, int top, bool text_left_aligned) {
  left = std::max(1, std::min(left, radar::kSize - layout.block_w - 1));
  top = std::max(1, std::min(top, radar::kSize - layout.block_h - 1));
  layout.on_right = text_left_aligned;
  layout.anchor_x = text_left_aligned ? left : left + layout.block_w;
  layout.top_y = top;
  return layout;
}

/**
 * Pick the tag position for the aircraft at (x, y) that collides least with
 * the obstacles (symbols, tags placed so far, fixed labels). Candidates, in
 * order of preference: beside the symbol (toward the center first) shifted in
 * half-line steps, above or below it, then beside it with a wider gap. The
 * default (toward the center, vertically centered) wins whenever it is free.
 */
TagLayout placeAircraftTag(int x, int y, const services::adsb::Aircraft& plane,
                           const Box* obstacles, size_t obstacle_count) {
  const TagLayout m = measureAircraftTag(plane);
  const bool preferred_right = x < radar::kCenterX;
  const int gap = radar::kAircraftNoseLenPx + radar::kAircraftTailHalfPx +
                  radar::kAircraftLabelGapPx;
  constexpr int kSymbolHalf = radar::kAircraftNoseLenPx + 2;
  constexpr int kOverlapWeight = 8;
  constexpr int kOffPanelWeight = 400;

  TagLayout best = tagAt(m, preferred_right ? x + gap : x - gap - m.block_w,
                         y - m.block_h / 2, preferred_right);
  int best_cost = INT32_MAX;
  auto consider = [&](int left, int top, bool text_left_aligned, int penalty) {
    if (penalty >= best_cost) {
      return;
    }
    const TagLayout cand = tagAt(m, left, top, text_left_aligned);
    const Box box = tagBox(cand);
    int cost = penalty + offPanelPx(box) * kOffPanelWeight;
    for (size_t i = 0; i < obstacle_count && cost < best_cost; ++i) {
      cost += overlapArea(box, obstacles[i]) * kOverlapWeight;
    }
    if (cost < best_cost) {
      best_cost = cost;
      best = cand;
    }
  };

  // 1. Beside the symbol, shifted up/down in half-line steps.
  static constexpr int8_t kSteps[] = {0, -1, 1, -2, 2, -3, 3, -4, 4, -5, 5, -6, 6};
  for (int side = 0; side < 2; ++side) {
    const bool right = side == 0 ? preferred_right : !preferred_right;
    const int left = right ? x + gap : x - gap - m.block_w;
    for (const int8_t step : kSteps) {
      consider(left, y - m.block_h / 2 + step * m.line_h / 2, right,
               abs(step) + side * 5);
    }
  }
  // 2. Above or below the symbol, slid sideways in quarter-width steps.
  static constexpr int8_t kSlides[] = {0, -1, 1, -2, 2};
  for (int below = 0; below < 2; ++below) {
    const int top = below ? y + kSymbolHalf + 2 : y - kSymbolHalf - 2 - m.block_h;
    for (const int8_t slide : kSlides) {
      consider(x - m.block_w / 2 + slide * m.block_w / 4, top, true,
               4 + abs(slide) + below);
    }
  }
  // 3. Beside the symbol with a wider gap (leader line makes the link clear).
  static constexpr int8_t kFarSteps[] = {0, -2, 2, -4, 4};
  for (int side = 0; side < 2; ++side) {
    const bool right = side == 0 ? preferred_right : !preferred_right;
    const int far_gap = gap + 14;
    const int left = right ? x + far_gap : x - far_gap - m.block_w;
    for (const int8_t step : kFarSteps) {
      consider(left, y - m.block_h / 2 + step * m.line_h / 2, right,
               9 + abs(step) + side * 3);
    }
  }
  // 4. Dense clusters: centered further out in eight directions, so a tag can
  //    reach free space beyond its neighbours (the leader line shows the link).
  static constexpr int8_t kDirX[] = {1, -1, 0, 0, 1, 1, -1, -1};
  static constexpr int8_t kDirY[] = {0, 0, -1, 1, -1, 1, -1, 1};
  static constexpr int8_t kRadii[] = {28, 42, 56};
  for (const int8_t r : kRadii) {
    for (size_t dir = 0; dir < sizeof(kDirX); ++dir) {
      const int len = (kDirX[dir] != 0 && kDirY[dir] != 0) ? r * 7 / 10 : r;
      const int cx = x + kDirX[dir] * (len + m.block_w / 2);
      const int cy = y + kDirY[dir] * (len + m.block_h / 2);
      consider(cx - m.block_w / 2, cy - m.block_h / 2, kDirX[dir] >= 0,
               14 + r / 7);
    }
  }

  // Edge case: every candidate reaches off the panel (aircraft right at the
  // rim). Nudge the winner toward the center until its corners fit.
  const int max_r = radar::kScreenRadiusPx - 1;
  const int max_r_sq = max_r * max_r;
  for (int step = 0; step < 80; ++step) {
    const Box b = tagBox(best);
    if (distSqFromCenter(b.x0, b.y0) <= max_r_sq &&
        distSqFromCenter(b.x1, b.y0) <= max_r_sq &&
        distSqFromCenter(b.x0, b.y1) <= max_r_sq &&
        distSqFromCenter(b.x1, b.y1) <= max_r_sq) {
      break;
    }
    const int mid_x = (b.x0 + b.x1) / 2;
    const int mid_y = (b.y0 + b.y1) / 2;
    best.anchor_x += mid_x < radar::kCenterX ? 1 : (mid_x > radar::kCenterX ? -1 : 0);
    best.top_y += mid_y < radar::kCenterY ? 1 : (mid_y > radar::kCenterY ? -1 : 0);
  }
  return best;
}

/**
 * Thin line from the symbol to its tag when placement moved the tag away from
 * the symbol, so it is clear which aircraft it belongs to.
 */
void drawTagLeader(int x, int y, const TagLayout& layout) {
  const Box b = tagBox(layout);
  // Nearest point of the tag box; a tag in its default spot is ~11 px away.
  const int ex = std::max<int>(b.x0, std::min<int>(x, b.x1 - 1));
  const int ey = std::max<int>(b.y0, std::min<int>(y, b.y1 - 1));
  const float dx = static_cast<float>(ex - x);
  const float dy = static_cast<float>(ey - y);
  const float len = sqrtf(dx * dx + dy * dy);
  constexpr float kStartGap = radar::kAircraftNoseLenPx + 2.0f;
  if (len < kStartGap + 6.0f) {
    return;  // tag sits right beside its symbol
  }
  const int sx = x + static_cast<int>(lroundf(dx / len * kStartGap));
  const int sy = y + static_cast<int>(lroundf(dy / len * kStartGap));
  if (s_sat_frame) {
    s_draw->drawWideLine(sx, sy, ex, ey, 1.0f, kOutlineColor);
  }
  s_draw->drawLine(sx, sy, ex, ey, kColorTagLeader);
}

void drawTagBackdrop(const TagLayout& layout) {
  constexpr int kPad = 2;
  const int left = layout.on_right ? layout.anchor_x
                                   : layout.anchor_x - layout.block_w;
  backdrop(left - kPad, layout.top_y - 1, layout.block_w + kPad * 2,
           layout.block_h + 2);
}

void drawAircraftTag(const TagLayout& layout,
                     const services::adsb::Aircraft& plane) {
  applyTagStyle();
  s_draw->setTextDatum(layout.on_right ? textdatum_t::top_left
                                       : textdatum_t::top_right);
  int ly = layout.top_y;

  const char* identity =
      plane.route[0] != '\0' ? plane.route : plane.callsign;
  if (identity[0] != '\0') {
    setLabelColor(radar::kColorLabel);
    s_draw->drawString(identity, layout.anchor_x, ly);
  }
  ly += layout.line_h;

  if (plane.type[0] != '\0') {
    setLabelColor(radar::kColorTagType);
    s_draw->drawString(plane.type, layout.anchor_x, ly);
  }
  ly += layout.line_h;

  if (plane.alt[0] != '\0') {
    setLabelColor(radar::kColorTagAltitude);
    s_draw->drawString(plane.alt, layout.anchor_x, ly);
  }
}

void applyFooterStyle() {
  initFooterMetrics();
  if (s_footer_use_vlw) {
    displayFontSetSmoothSize(*s_draw,
                             s_footer_vlw_size * configuredTextScale());
  } else {
    s_draw->setFont(&lgfx_fonts::Font0);
    applyBitmapTextScale(*s_draw);
  }
}

void fitFooterText(const char* source, char* out, size_t out_len,
                   int max_width) {
  if (out_len == 0) {
    return;
  }
  strncpy(out, source != nullptr ? source : "", out_len - 1);
  out[out_len - 1] = '\0';
  if (s_draw->textWidth(out) <= max_width) {
    return;
  }

  size_t length = strlen(out);
  while (length > 3) {
    length -= 1;
    out[length] = '\0';
    if (length >= 3) {
      out[length - 3] = '.';
      out[length - 2] = '.';
      out[length - 1] = '.';
    }
    if (s_draw->textWidth(out) <= max_width) {
      return;
    }
  }
}

void drawFooterLine(const char* text, int y, int max_width, uint16_t color) {
  if (text == nullptr || text[0] == '\0') {
    return;
  }
  applyFooterStyle();
  char fitted[32] = {};
  fitFooterText(text, fitted, sizeof(fitted), max_width);
  s_draw->setTextDatum(textdatum_t::top_center);
  s_draw->setTextColor(color, radar::kColorFooterBackground);
  s_draw->drawString(fitted, radar::kCenterX, y);
}

void drawFooter() {
  if (!services::settings::footerEnabled()) {
    return;
  }

  // The trapezoid follows the narrowing bottom edge of the round panel.
  s_draw->fillTriangle(28, radar::kFooterTopY, 212, radar::kFooterTopY, 168,
                       radar::kFooterBottomY,
                       radar::kColorFooterBackground);
  s_draw->fillTriangle(28, radar::kFooterTopY, 168, radar::kFooterBottomY, 72,
                       radar::kFooterBottomY,
                       radar::kColorFooterBackground);
  s_draw->drawFastHLine(44, radar::kFooterTopY, 152, radar::kColorGrid);

  if (services::settings::weatherEnabled()) {
    char weather[32] = {};
    services::weather::formatWeatherLine(weather, sizeof(weather));
    drawFooterLine(weather, radar::kFooterWeatherY, 176,
                   radar::kColorTagType);
  }

  char date_time[20] = {};
  services::weather::formatDateTimeLine(date_time, sizeof(date_time));
  const int time_y = services::settings::weatherEnabled()
                         ? radar::kFooterTimeY
                         : radar::kFooterTimeOnlyY;
  drawFooterLine(date_time, time_y, 128,
                 radar::kColorTagAltitude);
}

struct AircraftDrawItem {
  size_t index = 0;
  int x = 0;
  int y = 0;
  int dist_sq = 0;
};

struct BeyondDotDrawItem {
  int x = 0;
  int y = 0;
  int dist_sq = 0;
};

void sortDrawItemsFarFirst(AircraftDrawItem* items, size_t count) {
  for (size_t i = 1; i < count; ++i) {
    const AircraftDrawItem key = items[i];
    size_t j = i;
    while (j > 0 && items[j - 1].dist_sq < key.dist_sq) {
      items[j] = items[j - 1];
      --j;
    }
    items[j] = key;
  }
}

void sortBeyondDotsFarFirst(BeyondDotDrawItem* items, size_t count) {
  for (size_t i = 1; i < count; ++i) {
    const BeyondDotDrawItem key = items[i];
    size_t j = i;
    while (j > 0 && items[j - 1].dist_sq < key.dist_sq) {
      items[j] = items[j - 1];
      --j;
    }
    items[j] = key;
  }
}

void drawAircraft() {
  initLabelMetrics();

  const size_t n = services::adsb::aircraftCount();
  const services::adsb::Aircraft* planes = services::adsb::aircraftList();

  AircraftDrawItem items[services::adsb::kMaxAircraft];
  BeyondDotDrawItem dots[services::adsb::kMaxAircraft];
  size_t draw_count = 0;
  size_t dot_count = 0;

  for (size_t i = 0; i < n; ++i) {
    float dx_km = 0.0f;
    float dy_km = 0.0f;
    float dist_km = 0.0f;
    offsetKmFromCenter(planes[i].lat, planes[i].lon, &dx_km, &dy_km, &dist_km);

    if (isSymbolDistanceKm(dist_km)) {
      int x = 0;
      int y = 0;
      latLonToScreen(planes[i].lat, planes[i].lon, &x, &y);
      items[draw_count].index = i;
      items[draw_count].x = x;
      items[draw_count].y = y;
      items[draw_count].dist_sq = distSqFromCenter(x, y);
      ++draw_count;
      continue;
    }

    int dot_x = 0;
    int dot_y = 0;
    if (!beyondRingEdgeDotFromLatLon(planes[i].lat, planes[i].lon, &dot_x,
                                     &dot_y)) {
      continue;
    }
    dots[dot_count].x = dot_x;
    dots[dot_count].y = dot_y;
    dots[dot_count].dist_sq = distSqFromCenter(dot_x, dot_y);
    ++dot_count;
  }

  sortBeyondDotsFarFirst(dots, dot_count);
  for (size_t d = 0; d < dot_count; ++d) {
    drawBeyondRingDot(dots[d].x, dots[d].y);
  }

  sortDrawItemsFarFirst(items, draw_count);
  for (size_t d = 0; d < draw_count; ++d) {
    const size_t i = items[d].index;
    const int x = items[d].x;
    const int y = items[d].y;
    drawSpeedVector(x, y, planes[i].nose_deg, planes[i].track_deg,
                    planes[i].gs_knots, radar::kColorTrackVector);
    if (s_sat_frame) {
      drawHeadingTriangle(x, y, planes[i].nose_deg, kOutlineColor, 1.0f);
    }
    drawHeadingTriangle(x, y, planes[i].nose_deg, radar::kColorAircraft);
  }

  // Tag placement. Obstacles: every symbol, the fixed labels, the footer, and
  // each tag as it is placed. Aircraft nearest the center choose first.
  // Everything stays on the loop stack (~1.5 KB at the 64-aircraft cap).
  constexpr size_t kFixedObstacles = 5;
  Box obstacles[services::adsb::kMaxAircraft * 2 + kFixedObstacles];
  size_t obstacle_count = 0;
  // Cardinal letters at the bezel (approximate glyph boxes) and the footer.
  obstacles[obstacle_count++] = makeBox(radar::kCenterX - 9, 0, radar::kCenterX + 9, 18);
  obstacles[obstacle_count++] =
      makeBox(radar::kCenterX - 9, radar::kSize - 18, radar::kCenterX + 9, radar::kSize);
  obstacles[obstacle_count++] = makeBox(0, radar::kCenterY - 9, 16, radar::kCenterY + 9);
  obstacles[obstacle_count++] =
      makeBox(radar::kSize - 16, radar::kCenterY - 9, radar::kSize, radar::kCenterY + 9);
  if (services::settings::footerEnabled()) {
    obstacles[obstacle_count++] =
        makeBox(28, radar::kFooterTopY, 212, radar::kSize);
  }
  for (size_t d = 0; d < draw_count; ++d) {
    obstacles[obstacle_count++] = symbolBox(items[d].x, items[d].y);
  }

  struct TagPos {
    int16_t anchor_x;
    int16_t top_y;
    bool on_right;
  };
  TagPos tag_pos[services::adsb::kMaxAircraft];
  for (size_t k = draw_count; k-- > 0;) {  // items are sorted far-first
    const TagLayout layout =
        placeAircraftTag(items[k].x, items[k].y, planes[items[k].index],
                         obstacles, obstacle_count);
    obstacles[obstacle_count++] = tagBox(layout);
    tag_pos[k] = TagPos{static_cast<int16_t>(layout.anchor_x),
                        static_cast<int16_t>(layout.top_y), layout.on_right};
  }

  auto placedLayout = [&](size_t d) {
    TagLayout layout = measureAircraftTag(planes[items[d].index]);
    layout.anchor_x = tag_pos[d].anchor_x;
    layout.top_y = tag_pos[d].top_y;
    layout.on_right = tag_pos[d].on_right;
    return layout;
  };

  // Leaders first, then each tag as a unit (backdrop + text), far to near: if
  // tags still overlap, the later one covers the earlier cleanly instead of
  // two texts mixing. Tags nearest the center end up on top.
  for (size_t d = 0; d < draw_count; ++d) {
    drawTagLeader(items[d].x, items[d].y, placedLayout(d));
  }
  for (size_t d = 0; d < draw_count; ++d) {
    const TagLayout layout = placedLayout(d);
    if (s_sat_frame) {
      drawTagBackdrop(layout);
    }
    drawAircraftTag(layout, planes[items[d].index]);
  }
}

void applyCardinalStyle() {
  if (s_cardinal_use_vlw) {
    displayFontSetSmoothSize(*s_draw,
                             s_cardinal_vlw_size * configuredTextScale());
  } else {
    displayFontSetBitmap(*s_draw, s_cardinal_gfx);
    applyBitmapTextScale(*s_draw);
  }
}

void applyScaleStyle() {
  if (s_scale_use_vlw) {
    displayFontSetSmoothSize(*s_draw,
                             s_scale_vlw_size * configuredTextScale());
  } else {
    displayFontSetBitmap(*s_draw, s_scale_gfx);
    applyBitmapTextScale(*s_draw);
  }
}

void drawCardinalLabel(const char* text, int x, int y, textdatum_t datum) {
  applyCardinalStyle();
  s_draw->setTextDatum(datum);
  if (s_sat_frame) {
    const int w = s_draw->textWidth(text);
    const int h = s_draw->fontHeight();
    int left = x - w / 2;
    int top = y;
    if (datum == textdatum_t::middle_left) {
      left = x;
    } else if (datum == textdatum_t::middle_right) {
      left = x - w;
    }
    if (datum == textdatum_t::bottom_center) {
      top = y - h;
    } else if (datum == textdatum_t::middle_left ||
               datum == textdatum_t::middle_right) {
      top = y - h / 2;
    }
    backdrop(left - 2, top, w + 4, h);
  }
  setLabelColor(radar::kColorLabel);
  s_draw->drawString(text, x, y);
}

void drawScaleLabelWithBackground(const char* text, int x, int y) {
  applyScaleStyle();
  s_draw->setTextDatum(textdatum_t::middle_right);

  const int tw = s_draw->textWidth(text);
  const int th = s_draw->fontHeight();
  constexpr int kPadX = 3;
  constexpr int kPadY = 2;

  const int left = x - tw - kPadX;
  const int top = y - th / 2 - kPadY;

  s_draw->fillRect(left, top, tw + kPadX * 2, th + kPadY * 2,
                   radar::kColorBackground);
  s_draw->setTextColor(radar::kColorGrid, radar::kColorBackground);
  s_draw->drawString(text, x, y);
}

void drawGridRing(int cx, int cy, int r, uint16_t color) {
  if (r <= 0) {
    return;
  }
  const int thickness =
      std::max(1, static_cast<int>(radar::kGridStrokeHalfWidth * 2.0f));
  for (int i = 0; i < thickness && r - i > 0; ++i) {
    s_draw->drawCircle(cx, cy, r - i, color);
  }
}

void drawRings(int cx, int cy, int outer_radius) {
  for (int i = 1; i <= radar::kRingCount; ++i) {
    const int r = (outer_radius * i) / radar::kRingCount;
    drawGridRing(cx, cy, r, radar::kColorGrid);
  }
}

void drawCrosshairs(int cx, int cy, int radius, uint16_t color) {
  s_draw->drawWideLine(cx, cy - radius, cx, cy + radius,
                       radar::kGridStrokeHalfWidth, color);
  s_draw->drawWideLine(cx - radius, cy, cx + radius, cy,
                       radar::kGridStrokeHalfWidth, color);
}

void drawCenterDot(int cx, int cy) {
  if (s_sat_frame) {
    s_draw->fillSmoothCircle(cx, cy, radar::kCenterDotRadius + 2, kOutlineColor);
  }
  s_draw->fillSmoothCircle(cx, cy, radar::kCenterDotRadius, radar::kColorCenter);
}

void drawCardinalLabels() {
  const int cx = radar::kCenterX;
  const int cy = radar::kCenterY;
  const int edge = radar::kSize - 1;

  drawCardinalLabel("N", cx, radar::kCardinalNorthOffsetY, textdatum_t::top_center);
  drawCardinalLabel("S", cx, edge + radar::kCardinalSouthOffsetY,
                    textdatum_t::bottom_center);
  drawCardinalLabel("W", 0, cy, textdatum_t::middle_left);
  drawCardinalLabel("E", edge, cy, textdatum_t::middle_right);
}

int scaleLabelAnchorX(int cx, int outer_radius) {
  return cx + outer_radius - radar::kScaleGapFromOuterRing;
}

void drawScaleLabel(int cx, int cy, int outer_radius) {
  char scale_label[12];
  radar::formatCurrentRing3Label(scale_label, sizeof(scale_label));
  drawScaleLabelWithBackground(scale_label,
                               scaleLabelAnchorX(cx, outer_radius), cy);
}

/** Satellite image as the frame background; only possible in the sprite. */
bool drawMapBackground(LGFX_Sprite& sprite) {
  if (!services::settings::satelliteBackground()) {
    return false;
  }
  return services::satmap::drawBackground(
      sprite, services::location::lat(), services::location::lon(),
      radarViewHalfWidthKm(), services::settings::satelliteBrightnessPercent());
}

bool drawMapBackground(lgfx::LGFXBase&) { return false; }

void drawRainLayer(LGFX_Sprite& sprite) {
  if (services::settings::rainOverlay()) {
    services::rain::drawOverlay(sprite, services::location::lat(),
                                services::location::lon(),
                                radarViewHalfWidthKm());
  }
}

void drawRainLayer(lgfx::LGFXBase&) {}

template <typename Gfx>
void drawStaticGrid(Gfx& gfx) {
  initLabelMetrics();
  const DrawScope scope(gfx);
  displayFontEnsureLoaded(gfx);
  const int cx = radar::kCenterX;
  const int cy = radar::kCenterY;
  const int grid_r = radar::kGridOuterRadius;

  s_sat_frame = drawMapBackground(gfx);
  if (!s_sat_frame) {
    gfx.fillScreen(radar::kColorBackground);
  }
  drawRainLayer(gfx);
  if (!s_sat_frame) {
    // The satellite view has no grid: the image itself shows where things are.
    drawRings(cx, cy, grid_r);
    drawCrosshairs(cx, cy, grid_r, radar::kColorGrid);
  }
  initPalette();
  runway::drawLargeAirportRunways(gfx, s_sat_frame ? &backdrop : nullptr);
  drawCenterDot(cx, cy);
  drawCardinalLabels();
  if (!s_sat_frame) {
    drawScaleLabel(cx, cy, grid_r);  // labels ring 3, meaningless without rings
  }
  gfx.setTextDatum(textdatum_t::top_left);
}

bool ensureFrameSprite() {
  if (s_frame_ready) {
    return true;
  }
  s_frame.setColorDepth(16);
  if (!s_frame.createSprite(radar::kSize, radar::kSize)) {
    Serial.println("radar: frame sprite alloc failed");
    return false;
  }
  s_frame_ready = true;
  return true;
}

// Double-buffered frame: composite the grid AND aircraft into the off-screen
// sprite, then blit it to the panel in a single pushSprite. Because the panel
// is updated in one pass, labels never show an erase/redraw gap — no flicker.
void renderFrame() {
  drawStaticGrid(s_frame);  // opens its own DrawScope(s_frame)
  {
    const DrawScope scope(s_frame);
    drawAircraft();
    drawFooter();
  }
  s_frame.pushSprite(0, 0);
  tft.setTextDatum(textdatum_t::top_left);
}

}  // namespace

float radarViewHalfWidthKm() {
  return radar::rangeCurrent().outer_km * (radar::kSize / 2.0f) /
         static_cast<float>(radar::kGridOuterRadius);
}

uint8_t* radarFrameScratch(size_t* len) {
  if (!ensureFrameSprite()) {
    *len = 0;
    return nullptr;
  }
  *len = static_cast<size_t>(radar::kSize) * radar::kSize * 2;
  return static_cast<uint8_t*>(s_frame.getBuffer());
}

bool radarFrameRow(int y, uint16_t* out) {
  if (!s_frame_ready || y < 0 || y >= radar::kSize) {
    return false;
  }
  const auto* row =
      static_cast<const uint16_t*>(s_frame.getBuffer()) + y * radar::kSize;
  for (int x = 0; x < radar::kSize; ++x) {
    out[x] = ui::pixels::unswap(row[x]);
  }
  return true;
}

void radarDisplayDraw() {
  initPalette();
  initLabelMetrics();

  if (ensureFrameSprite()) {
    renderFrame();
    return;
  }

  // Fallback when the sprite can't be allocated: draw straight to the panel.
  const DrawScope scope(tft);
  s_sat_frame = false;
  drawStaticGrid(tft);
  drawAircraft();
  drawFooter();
  tft.setTextDatum(textdatum_t::top_left);
}

void radarDisplayRefreshAircraft() {
  initPalette();

  if (ensureFrameSprite()) {
    renderFrame();
    return;
  }

  radarDisplayDraw();
}

void radarProjectionDiagnostics(Print& out) {
  char buf[224];
  const float outer_km = radar::rangeCurrent().outer_km;

  snprintf(buf, sizeof(buf),
           "<tr><th>Longitude scale</th><td>%.4f &mdash; 1&deg; of longitude "
           "spans %.1f km here</td></tr>",
           lonScale(), kKmPerDeg * lonScale());
  out.print(buf);
  snprintf(buf, sizeof(buf),
           "<tr><th>Plot scale</th><td>%.2f px/km, rim dots beyond %.2f km"
           "</td></tr><tr><th>Frame sprite</th><td>%s</td></tr></table>",
           static_cast<double>(radar::kGridOuterRadius) / outer_km,
           symbolMaxKm(),
           s_frame_ready ? "allocated (115 KB)"
                         : "NOT allocated &mdash; drawing direct to panel");
  out.print(buf);

  const size_t count = services::adsb::aircraftCount();
  const services::adsb::Aircraft* list = services::adsb::aircraftList();
  snprintf(buf, sizeof(buf), "<h2>Aircraft (%u)</h2>",
           static_cast<unsigned>(count));
  out.print(buf);
  if (count == 0) {
    out.print(F("<p><small>None in range right now.</small></p>"));
    return;
  }

  out.print(F("<div class=\"wrap\"><table class=\"grid\"><tr><th>Callsign</th>"
              "<th>Hex</th><th>Latitude</th><th>Longitude</th><th>East km</th>"
              "<th>North km</th><th>Distance</th><th>Screen</th><th>Drawn as"
              "</th><th>Route</th><th>Type</th><th>Alt</th></tr>"));
  for (size_t i = 0; i < count; ++i) {
    float dx = 0.0f;
    float dy = 0.0f;
    float dist = 0.0f;
    offsetKmFromCenter(list[i].lat, list[i].lon, &dx, &dy, &dist);
    int x = 0;
    int y = 0;
    latLonToScreen(list[i].lat, list[i].lon, &x, &y);
    snprintf(buf, sizeof(buf),
             "<tr><td>%s</td><td>%s</td><td>%.5f</td><td>%.5f</td>"
             "<td>%+.2f</td><td>%+.2f</td><td>%.2f km</td><td>%d, %d</td>"
             "<td>%s</td><td>%s</td><td>%s</td><td>%s</td></tr>",
             list[i].callsign, list[i].hex, list[i].lat, list[i].lon, dx, dy,
             dist, x, y, dist > symbolMaxKm() ? "rim dot" : "symbol",
             list[i].route[0] != '\0' ? list[i].route : "&mdash;",
             list[i].type[0] != '\0' ? list[i].type : "&mdash;",
             list[i].alt[0] != '\0' ? list[i].alt : "&mdash;");
    out.print(buf);
  }
  out.print(F("</table></div>"));
}

}  // namespace ui
