#pragma once

#include <Arduino.h>

bool startBleTransport(const String &device_name, const String &token);
bool bleTransportReady();
bool bleTransportValidated();
void bleTransportLoop();
bool bleTransportRequest(const char *method, const String &path,
                         const char *body, String &response,
                         uint32_t timeout_ms);
