#pragma once

#include <Arduino.h>

#include "services/device_config.h"

namespace DisplayControl {

struct RemoteState {
  bool online = false;
  bool ready = false;
  int brightness = 50;
  int volume = 20;
  bool muted = false;
  char input[8] = "usbc";
  char message[48] = "Connecting";
  bool brightness_pending = false;
  bool volume_pending = false;
  bool mute_pending = false;
  bool input_pending = false;
  uint32_t revision = 0;
};

void startRemote(const DeviceConfig &config);
DeviceConfig getDesktopConfig();
RemoteState getRemoteState();
bool queueNumericControl(const char *control, int value, bool final_value = true);
bool queueBooleanControl(const char *control, bool value);
bool queueStringControl(const char *control, const char *value);

}  // namespace DisplayControl
