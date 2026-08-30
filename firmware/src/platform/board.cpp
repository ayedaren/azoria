#include "platform/board.h"

#include <Arduino.h>
#include <esp_display_panel.hpp>
#include <esp_lcd_panel_rgb.h>
#include <new>

namespace {

#ifndef AZORIA_RGB_CLOCK_HZ
#define AZORIA_RGB_CLOCK_HZ 16000000
#endif

#ifndef AZORIA_RGB_BOUNCE_BUFFER_SIZE
#define AZORIA_RGB_BOUNCE_BUFFER_SIZE (480 * 60)
#endif

esp_panel::board::Board *hardware = nullptr;
esp_panel::drivers::LCD *lcd = nullptr;
esp_panel::drivers::Backlight *backlight = nullptr;
esp_panel::drivers::BusI2C *touch_bus = nullptr;
esp_lcd_panel_io_handle_t touch_io = nullptr;

constexpr uint8_t kTouchDiagnosticRegisters[] = {
    0x00,
    0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
    0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA8,
};
uint8_t touch_preinit_values[sizeof(kTouchDiagnosticRegisters)]{};
bool touch_preinit_valid[sizeof(kTouchDiagnosticRegisters)]{};

void captureTouchPreinitDiagnostics() {
  for (size_t index = 0; index < sizeof(kTouchDiagnosticRegisters); ++index) {
    touch_preinit_values[index] = 0xFF;
    touch_preinit_valid[index] =
        esp_lcd_panel_io_rx_param(touch_io, kTouchDiagnosticRegisters[index],
                                  &touch_preinit_values[index], 1) == ESP_OK;
  }
}

bool writeTouchRegister(uint8_t reg, uint8_t value) {
  return esp_lcd_panel_io_tx_param(touch_io, reg, &value, 1) == ESP_OK;
}

bool applyOfficialFT6336USettings() {
  // VIEWE's official FT62XX driver for this board writes only these four
  // working-mode parameters. In particular, do not apply the generic FT5x06
  // tuning block: several of its addresses are reserved on FT6336U.
  constexpr struct {
    uint8_t reg;
    uint8_t value;
  } settings[] = {
      {0x00, 0x00},  // Working mode
      {0xA4, 0x00},  // Polling mode
      {0x80, 22},    // VIEWE touch threshold
      {0x88, 12},    // VIEWE active report period
  };

  for (const auto &setting : settings) {
    if (!writeTouchRegister(setting.reg, setting.value)) return false;
  }
  return true;
}

bool beginVieweFT6336U() {
  esp_lcd_panel_io_i2c_config_t io_config{};
  io_config.dev_addr = 0x38;
  io_config.control_phase_bytes = 1;
  io_config.dc_bit_offset = 0;
  io_config.lcd_cmd_bits = 8;
  io_config.flags.disable_control_phase = 1;

  touch_bus = new (std::nothrow)
      esp_panel::drivers::BusI2C(41, 40, io_config);
  if (!touch_bus || !touch_bus->begin()) {
    Serial.println("VIEWE touch I2C bus begin failed");
    return false;
  }
  touch_io = touch_bus->getControlPanelHandle();
  if (!touch_io) {
    Serial.println("VIEWE touch I2C panel IO missing");
    return false;
  }
  captureTouchPreinitDiagnostics();
  if (!applyOfficialFT6336USettings()) {
    Serial.println("VIEWE official FT6336U settings failed");
    return false;
  }
  return true;
}

}  // namespace

namespace Board {

bool begin() {
  hardware = new (std::nothrow) esp_panel::board::Board();
  if (!hardware) {
    Serial.println("VIEWE board allocation failed");
    return false;
  }

  if (!hardware->init()) {
    Serial.println("VIEWE board init failed");
    return false;
  }

  lcd = hardware->getLCD();
  backlight = hardware->getBacklight();
  if (!lcd || !backlight) {
    Serial.println("VIEWE LCD/backlight driver missing");
    return false;
  }

  auto *bus = lcd->getBus();
  if (bus && bus->getBasicAttributes().type == ESP_PANEL_BUS_TYPE_RGB) {
    auto *rgb_bus = static_cast<esp_panel::drivers::BusRGB *>(bus);
    if (!rgb_bus->configRGB_FreqHz(AZORIA_RGB_CLOCK_HZ) ||
        !rgb_bus->configRGB_BounceBufferSize(AZORIA_RGB_BOUNCE_BUFFER_SIZE)) {
      Serial.println("VIEWE stable RGB timing configuration failed");
      return false;
    }
  }

  if (!lcd->configFrameBufferNumber(2)) {
    Serial.println("VIEWE double frame buffer configuration failed");
    return false;
  }
  if (!hardware->begin()) {
    Serial.println("VIEWE board begin failed");
    return false;
  }
  if (!beginVieweFT6336U()) {
    Serial.println("VIEWE FT6336U settings failed");
    return false;
  }

  setBacklight(220);
  Serial.printf("RGB profile: %d Hz, bounce %d\n",
                AZORIA_RGB_CLOCK_HZ, AZORIA_RGB_BOUNCE_BUFFER_SIZE);
  Serial.printf(
      "RGB SDK: XIP=%d, CACHE_LINE=%d, OPT_PERF=%d, LCD_ISR_IRAM=%d, "
      "RESTART_EVERY_VSYNC=%d\n",
#if defined(CONFIG_SPIRAM_XIP_FROM_PSRAM)
      1,
#else
      0,
#endif
#if defined(CONFIG_ESP32S3_DATA_CACHE_LINE_SIZE)
      CONFIG_ESP32S3_DATA_CACHE_LINE_SIZE,
#else
      0,
#endif
#if defined(CONFIG_COMPILER_OPTIMIZATION_PERF)
      1,
#else
      0,
#endif
#if defined(CONFIG_LCD_RGB_ISR_IRAM_SAFE)
      1,
#else
      0,
#endif
#if defined(CONFIG_LCD_RGB_RESTART_IN_VSYNC)
      1
#else
      0
#endif
  );
  Serial.println("VIEWE UEDX48480040E-WB-A ready");
  return true;
}

bool readTouch(uint16_t &x, uint16_t &y) {
  TouchPoint point;
  if (readTouches(&point, 1) <= 0) return false;
  x = point.x;
  y = point.y;
  return true;
}

int readTouches(TouchPoint *points, int max_points) {
  if (!touch_io || !points || max_points <= 0) return -1;

  constexpr uint8_t kTouchStatusRegister = 0x02;
  constexpr uint8_t kFirstPointRegister = 0x03;
  constexpr int kControllerMaxPoints = 2;
  constexpr uint8_t kEventPressDown = 0;
  constexpr uint8_t kEventContact = 2;

  uint8_t raw_status = 0;
  if (esp_lcd_panel_io_rx_param(touch_io, kTouchStatusRegister,
                                &raw_status, 1) != ESP_OK) {
    return -1;
  }
  const int count = raw_status & 0x0F;
  if (count == 0) return 0;
  if (count > kControllerMaxPoints) return -1;

  // Always read every point reported by the controller. Point 0 can be a
  // trailing lift-up record while point 1 is still an active contact.
  uint8_t raw[kControllerMaxPoints * 6]{};
  if (esp_lcd_panel_io_rx_param(touch_io, kFirstPointRegister, raw,
                                count * 6) != ESP_OK) {
    return -1;
  }

  int active_points = 0;
  for (int index = 0; index < count && active_points < max_points; ++index) {
    const uint8_t *point = raw + index * 6;
    // FT6336U Pn_XH[7:6] is the event flag:
    //   0 = press down, 1 = lift up, 2 = contact, 3 = no event.
    // A lift-up record may retain its previous coordinates. Treating every
    // TD_STATUS point as pressed turns that stale record into a phantom click.
    const uint8_t event = (point[0] >> 6) & 0x03;
    if (event != kEventPressDown && event != kEventContact) continue;

    const uint16_t x =
        (static_cast<uint16_t>(point[0] & 0x0F) << 8) | point[1];
    const uint16_t y =
        (static_cast<uint16_t>(point[2] & 0x0F) << 8) | point[3];
    points[active_points].x = static_cast<uint16_t>(constrain(x, 0, 479));
    points[active_points].y = static_cast<uint16_t>(constrain(y, 0, 479));
    points[active_points].strength = -1;
    ++active_points;
  }
  return active_points;
}

void printTouchDiagnostics() {
  if (!touch_io) {
    Serial.println("TOUCH_DIAG,NO_IO");
    return;
  }

  Serial.print("TOUCH_PREINIT");
  for (size_t index = 0; index < sizeof(kTouchDiagnosticRegisters); ++index) {
    if (touch_preinit_valid[index]) {
      Serial.printf(",%02X=%02X", kTouchDiagnosticRegisters[index],
                    touch_preinit_values[index]);
    } else {
      Serial.printf(",%02X=ERR", kTouchDiagnosticRegisters[index]);
    }
  }
  Serial.println();

  Serial.print("TOUCH_DIAG");
  for (uint8_t reg : kTouchDiagnosticRegisters) {
    uint8_t value = 0xFF;
    esp_err_t result = esp_lcd_panel_io_rx_param(touch_io, reg, &value, 1);
    if (result == ESP_OK) {
      Serial.printf(",%02X=%02X", reg, value);
    } else {
      Serial.printf(",%02X=ERR", reg);
    }
  }
  Serial.println();
}

void setBacklight(uint8_t value) {
  if (!backlight) return;
  int percent = (static_cast<int>(value) * 100 + 127) / 255;
  backlight->setBrightness(percent);
}

void *frameBuffer(uint8_t index) {
  return lcd ? lcd->getFrameBufferByIndex(index) : nullptr;
}

bool switchFrameBuffer(void *buffer) {
  return lcd && buffer && lcd->switchFrameBufferTo(buffer);
}

bool restartRgbScan() {
  if (!lcd || !lcd->getBus() ||
      lcd->getBus()->getBasicAttributes().type != ESP_PANEL_BUS_TYPE_RGB) {
    return false;
  }
  esp_lcd_panel_handle_t panel = lcd->getRefreshPanelHandle();
  return panel && esp_lcd_rgb_panel_restart(panel) == ESP_OK;
}

bool attachRefreshFinishCallback(RefreshFinishCallback callback, void *user_data) {
  return lcd && callback && lcd->attachRefreshFinishCallback(callback, user_data);
}

}  // namespace Board
