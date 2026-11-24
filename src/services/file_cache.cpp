#include "services/file_cache.h"

#include <HTTPClient.h>
#include <WiFiClientSecure.h>

namespace services::files {
namespace {

bool s_mounted = false;
bool s_mount_failed = false;

void poll(PollFn fn) {
  if (fn != nullptr) {
    fn();
  }
}

}  // namespace

bool mount() {
  if (s_mounted) {
    return true;
  }
  if (s_mount_failed) {
    return false;
  }
  // Partition label "spiffs" from partitions/plane_radar.csv; format on first boot.
  s_mounted = LittleFS.begin(true, "/littlefs", 4, "spiffs");
  if (!s_mounted) {
    s_mount_failed = true;
    Serial.println("files: LittleFS mount failed");
  } else {
    Serial.printf("files: LittleFS %u/%u KB used\n",
                  static_cast<unsigned>(LittleFS.usedBytes() / 1024),
                  static_cast<unsigned>(LittleFS.totalBytes() / 1024));
  }
  return s_mounted;
}

bool mounted() { return s_mounted; }

int download(const char* url, const char* path, unsigned long timeout_ms,
             PollFn poll_fn, size_t* bytes) {
  if (bytes != nullptr) {
    *bytes = 0;
  }
  if (!mount()) {
    return HTTPC_ERROR_TOO_LESS_RAM;
  }

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, url)) {
    return HTTPC_ERROR_CONNECTION_REFUSED;
  }
  http.setConnectTimeout(static_cast<int32_t>(timeout_ms));
  http.setTimeout(static_cast<uint16_t>(timeout_ms));

  poll(poll_fn);
  const int code = http.GET();
  poll(poll_fn);
  if (code != HTTP_CODE_OK) {
    http.end();
    return code;
  }

  String tmp = path;
  tmp += ".part";
  File file = LittleFS.open(tmp, FILE_WRITE);
  if (!file) {
    http.end();
    return HTTPC_ERROR_STREAM_WRITE;
  }
  const int written = http.writeToStream(&file);
  file.close();
  http.end();
  poll(poll_fn);

  if (written <= 0) {
    LittleFS.remove(tmp);
    return written < 0 ? written : HTTPC_ERROR_NO_STREAM;
  }
  LittleFS.remove(path);
  if (!LittleFS.rename(tmp, path)) {
    LittleFS.remove(tmp);
    return HTTPC_ERROR_STREAM_WRITE;
  }
  if (bytes != nullptr) {
    *bytes = static_cast<size_t>(written);
  }
  return HTTP_CODE_OK;
}

size_t totalBytes() { return s_mounted ? LittleFS.totalBytes() : 0; }

size_t usedBytes() { return s_mounted ? LittleFS.usedBytes() : 0; }

}  // namespace services::files
