#include "services/device_config.h"

#include <Preferences.h>

namespace {
constexpr char kNamespace[] = "azoria";

bool isPrivateIpv4(const IPAddress &address) {
  const uint8_t first = address[0];
  const uint8_t second = address[1];
  return first == 10 || first == 127 ||
         (first == 169 && second == 254) ||
         (first == 172 && second >= 16 && second <= 31) ||
         (first == 192 && second == 168);
}

bool isLocalDesktopHost(const String &host) {
  if (host.isEmpty()) return true;
  IPAddress address;
  return address.fromString(host) && isPrivateIpv4(address);
}
}

bool DeviceConfig::valid() const {
  return !ssid.isEmpty() && token.length() >= 20 && port > 0 &&
         isLocalDesktopHost(host);
}

bool loadDeviceConfig(DeviceConfig &config) {
  Preferences preferences;
  if (!preferences.begin(kNamespace, true)) return false;
  config.ssid = preferences.getString("ssid");
  config.password = preferences.getString("pass");
  config.host = preferences.getString("host");
  config.port = preferences.getUShort("port", 8732);
  config.token = preferences.getString("token");
  preferences.end();
  return config.valid();
}

void saveDeviceConfig(const DeviceConfig &config) {
  Preferences preferences;
  preferences.begin(kNamespace, false);
  preferences.putString("ssid", config.ssid);
  preferences.putString("pass", config.password);
  preferences.putString("host", config.host);
  preferences.putUShort("port", config.port);
  preferences.putString("token", config.token);
  preferences.end();
}

void clearDeviceConfig() {
  Preferences preferences;
  preferences.begin(kNamespace, false);
  preferences.clear();
  preferences.end();
}
