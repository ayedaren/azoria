#pragma once

#include <Arduino.h>

struct DeviceConfig {
  String ssid;
  String password;
  String host;
  uint16_t port = 8732;
  String token;

  bool valid() const;
};

bool loadDeviceConfig(DeviceConfig &config);
void saveDeviceConfig(const DeviceConfig &config);
void clearDeviceConfig();
