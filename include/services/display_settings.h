#pragma once

#include <cstddef>

namespace services::settings {

constexpr size_t kOtaPasswordMaxLen = 32;
constexpr int kTextScaleMinPercent = 80;
constexpr int kTextScaleMaxPercent = 130;
constexpr int kTextScaleDefaultPercent = 110;
constexpr int kSatBrightnessMinPercent = 20;
constexpr int kSatBrightnessMaxPercent = 100;
constexpr int kSatBrightnessDefaultPercent = 100;

/** Load persistent display and OTA settings from NVS. */
void init();

bool footerEnabled();
bool weatherEnabled();
bool temperatureFahrenheit();
bool use24HourClock();
int textScalePercent();
/** Satellite image instead of the dark radar background (grid is hidden). */
bool satelliteBackground();
/** Satellite image brightness, so aircraft and tags stay readable. */
int satelliteBrightnessPercent();
/** Semi-transparent rain radar layer, in both background modes. */
bool rainOverlay();
const char* otaPassword();

/**
 * Store web-portal values. An empty OTA password keeps the current password so
 * the portal never needs to echo the stored secret into its HTML.
 */
void saveFromPortal(const char* footer_checkbox, const char* weather_checkbox,
                    const char* fahrenheit_checkbox,
                    const char* clock24_checkbox,
                    const char* text_scale_percent_value,
                    const char* satellite_checkbox,
                    const char* sat_brightness_percent_value,
                    const char* rain_checkbox,
                    const char* ota_password_value);

/** Restore defaults during a full BOOT-button reset. */
void clear();

}  // namespace services::settings
