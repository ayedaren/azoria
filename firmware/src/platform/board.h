#pragma once

#include <stdint.h>

namespace Board {

using RefreshFinishCallback = bool (*)(void *user_data);

struct TouchPoint {
  uint16_t x;
  uint16_t y;
  int16_t strength;
};

bool begin();
// Returns the number of valid points, 0 for a successful no-touch read, and
// -1 for an I2C/protocol/argument error.
int readTouches(TouchPoint *points, int max_points);
bool readTouch(uint16_t &x, uint16_t &y);
void printTouchDiagnostics();
void setBacklight(uint8_t value);
void *frameBuffer(uint8_t index);
bool switchFrameBuffer(void *buffer);
bool restartRgbScan();
bool attachRefreshFinishCallback(RefreshFinishCallback callback, void *user_data);

}  // namespace Board
