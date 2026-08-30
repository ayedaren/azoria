#include "services/usb_provisioning.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <cstring>

#include "services/device_config.h"
#include "platform/board.h"

namespace {

String input;
constexpr uint16_t kScreenWidth = 480;
constexpr uint16_t kScreenHeight = 480;
constexpr size_t kScreenshotBytes =
    static_cast<size_t>(kScreenWidth) * kScreenHeight * sizeof(uint16_t);
uint8_t *screenshot_copy = nullptr;

int hexValue(char value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  if (value >= 'A' && value <= 'F') return value - 'A' + 10;
  return -1;
}

String urlDecode(const String &value) {
  String result;
  result.reserve(value.length());
  for (size_t i = 0; i < value.length(); ++i) {
    if (value[i] == '+') {
      result += ' ';
    } else if (value[i] == '%' && i + 2 < value.length()) {
      int high = hexValue(value[i + 1]);
      int low = hexValue(value[i + 2]);
      if (high >= 0 && low >= 0) {
        result += static_cast<char>((high << 4) | low);
        i += 2;
      } else {
        result += value[i];
      }
    } else {
      result += value[i];
    }
  }
  return result;
}

String urlEncode(const String &value) {
  constexpr char hex[] = "0123456789ABCDEF";
  String result;
  result.reserve(value.length() * 2);
  for (size_t i = 0; i < value.length(); ++i) {
    uint8_t byte = static_cast<uint8_t>(value[i]);
    if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
        (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' ||
        byte == '.' || byte == '~') {
      result += static_cast<char>(byte);
    } else {
      result += '%';
      result += hex[byte >> 4];
      result += hex[byte & 0x0F];
    }
  }
  return result;
}

String formValue(const String &form, const char *key) {
  String marker = String(key) + "=";
  int start = form.indexOf(marker);
  while (start > 0 && form[start - 1] != '&') start = form.indexOf(marker, start + 1);
  if (start < 0) return {};
  start += marker.length();
  int end = form.indexOf('&', start);
  if (end < 0) end = form.length();
  return urlDecode(form.substring(start, end));
}

bool waitForWiFi(uint32_t timeout_ms) {
  uint32_t started = millis();
  while (WiFi.status() != WL_CONNECTED &&
         millis() - started < timeout_ms) {
    delay(50);
  }
  return WiFi.status() == WL_CONNECTED;
}

void restoreSavedWiFi(const DeviceConfig &saved) {
  Serial.println("AZORIA_RESTORING saved Wi-Fi");
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(false, true, 1000);
  WiFi.mode(WIFI_STA);
  WiFi.begin(saved.ssid.c_str(), saved.password.c_str());
  if (waitForWiFi(20000)) {
    WiFi.setAutoReconnect(true);
    Serial.printf("AZORIA_WIFI_RESTORED ip=%s\n",
                  WiFi.localIP().toString().c_str());
    return;
  }
  WiFi.disconnect(false, true, 1000);
  WiFi.setAutoReconnect(false);
  Serial.println("AZORIA_RESTORE_FAILED saved Wi-Fi is unavailable");
}

void scanNetworks() {
  Serial.println("AZORIA_SCAN_STARTED");
  WiFi.mode(WIFI_STA);
  int count = WiFi.scanNetworks(false, false);
  if (count < 0) {
    Serial.println("AZORIA_ERROR Wi-Fi scan failed");
    Serial.println("AZORIA_NETWORKS_DONE");
    return;
  }

  String seen[24];
  size_t seen_count = 0;
  for (int index = 0; index < count && seen_count < 24; ++index) {
    String ssid = WiFi.SSID(index);
    if (ssid.isEmpty()) continue;
    bool duplicate = false;
    for (size_t prior = 0; prior < seen_count; ++prior) {
      if (seen[prior] == ssid) {
        duplicate = true;
        break;
      }
    }
    if (duplicate) continue;
    seen[seen_count++] = ssid;
    Serial.printf("AZORIA_NETWORK ssid=%s&rssi=%ld&secure=%d\n",
                  urlEncode(ssid).c_str(), static_cast<long>(WiFi.RSSI(index)),
                  WiFi.encryptionType(index) == WIFI_AUTH_OPEN ? 0 : 1);
  }
  WiFi.scanDelete();
  Serial.println("AZORIA_NETWORKS_DONE");
}

void sendScreenshot(uint8_t buffer_index) {
  void *source = Board::frameBuffer(buffer_index);
  if (!source) {
    Serial.println("AZORIA_SCREENSHOT_ERROR framebuffer unavailable");
    return;
  }
  if (!screenshot_copy) {
    screenshot_copy = static_cast<uint8_t *>(
        heap_caps_malloc(kScreenshotBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  }
  if (!screenshot_copy) {
    Serial.println("AZORIA_SCREENSHOT_ERROR capture buffer unavailable");
    return;
  }

  // Copy before announcing the payload so the host can synchronise on the
  // header even when normal provisioning/status logs are sharing USB CDC.
  memcpy(screenshot_copy, source, kScreenshotBytes);
  Serial.printf("AZORIA_SCREENSHOT_BEGIN %u %u RGB565LE %u\n", kScreenWidth,
                kScreenHeight, static_cast<unsigned>(kScreenshotBytes));
  Serial.flush();
  Serial.write(screenshot_copy, kScreenshotBytes);
  Serial.flush();
  Serial.println("AZORIA_SCREENSHOT_END");
}

void handleLine(String line) {
  line.trim();
  if (line == "AZORIA_IDENTIFY") {
    Serial.println("AZORIA_TOUCH_V1");
    return;
  }
  if (line == "AZORIA_SCREENSHOT" || line == "AZORIA_SCREENSHOT 0") {
    sendScreenshot(0);
    return;
  }
  if (line == "AZORIA_SCREENSHOT 1") {
    sendScreenshot(1);
    return;
  }
  if (line == "AZORIA_REBOOT") {
    Serial.println("AZORIA_OK rebooting");
    Serial.flush();
    delay(100);
    ESP.restart();
    return;
  }
  if (line == "AZORIA_SCAN") {
    scanNetworks();
    return;
  }
  constexpr char prefix[] = "AZORIA_CONFIG ";
  if (!line.startsWith(prefix)) return;

  String form = line.substring(sizeof(prefix) - 1);
  DeviceConfig config;
  config.ssid = formValue(form, "ssid");
  config.password = formValue(form, "pass");
  config.host = formValue(form, "host");
  config.port = static_cast<uint16_t>(formValue(form, "port").toInt());
  config.token = formValue(form, "token");
  if (!config.valid()) {
    Serial.println("AZORIA_ERROR invalid configuration");
    return;
  }

  DeviceConfig saved_config;
  bool has_saved_config = loadDeviceConfig(saved_config);
  Serial.println("AZORIA_CONNECTING testing Wi-Fi credentials");
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(false, true, 1000);
  WiFi.begin(config.ssid.c_str(), config.password.c_str());
  if (!waitForWiFi(20000)) {
    WiFi.disconnect(false, true, 1000);
    Serial.println("AZORIA_ERROR Wi-Fi connection failed; check the password");
    Serial.flush();
    // The new credentials were only tested and were never saved. Bring the
    // previous network back so a typo cannot strand an already configured
    // controller or interrupt its normal Desktop connection.
    if (has_saved_config) restoreSavedWiFi(saved_config);
    return;
  }

  WiFi.setAutoReconnect(true);
  saveDeviceConfig(config);
  Serial.printf("AZORIA_WIFI_OK ip=%s\n", WiFi.localIP().toString().c_str());
  Serial.println("AZORIA_OK configuration saved");
  Serial.flush();
  delay(250);
  ESP.restart();
}

}  // namespace

void beginUsbProvisioning() {
  input.reserve(512);
  Serial.println("AZORIA_READY usb provisioning");
}

void usbProvisioningLoop() {
  static uint32_t last_ready = 0;
  if (millis() - last_ready >= 5000) {
    Serial.println("AZORIA_READY usb provisioning");
    last_ready = millis();
  }
  while (Serial.available()) {
    char value = static_cast<char>(Serial.read());
    if (value == '\r') continue;
    if (value == '\n') {
      handleLine(input);
      input = "";
    } else if (input.length() < 768) {
      input += value;
    } else {
      input = "";
      Serial.println("AZORIA_ERROR command too long");
    }
  }
}
