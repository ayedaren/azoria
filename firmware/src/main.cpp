#include <Arduino.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lvgl.h>
#include <time.h>

#include "modules/kvm/screen.h"
#include "modules/kvm/service.h"
#include "platform/board.h"
#include "services/device_config.h"
#include "services/ble_transport.h"
#include "services/usb_provisioning.h"
#include "ui/app_ui.h"

namespace {

constexpr uint32_t kWidth = 480;
constexpr uint32_t kHeight = 480;
constexpr uint32_t kBufferPixels = kWidth * kHeight;
lv_disp_draw_buf_t draw_buffer;
lv_color_t *buffer1 = nullptr;
lv_color_t *buffer2 = nullptr;
bool provisioning = false;
bool have_saved_config = false;
bool remote_started = false;
DeviceConfig saved_config;
uint32_t next_wifi_retry = 0;
uint32_t next_runtime_diagnostics = 0;
volatile uint32_t display_refresh_count = 0;
volatile TickType_t display_last_refresh_tick = 0;
volatile TickType_t display_max_refresh_gap_ticks = 0;
TickType_t display_max_flush_wait_ticks = 0;
uint32_t display_recovery_count = 0;
uint32_t display_full_redraw_count = 0;
bool display_full_redraw_requested = false;
bool display_full_redraw_pending = false;
constexpr uint32_t kRuntimeDiagnosticIntervalMs = 60000;
constexpr TickType_t kFrameBoundaryTimeout = pdMS_TO_TICKS(250);
// The FT6336U is sampled every 8 ms. A real contact persists across reports,
// while electrical noise and stale event records are normally isolated.
// Confirm a new contact twice and a release three times at the input-driver
// boundary. LVGL still owns all click, drag and gesture state.
constexpr uint8_t kTouchPressConfirmSamples = 2;
constexpr uint8_t kTouchReleaseConfirmSamples = 3;
Board::TouchPoint last_touch_point{};
uint8_t touch_press_samples = 0;
uint8_t touch_release_samples = 0;
bool touch_is_pressed = false;

IRAM_ATTR bool displayRefreshFinished(void *user_data) {
  TickType_t now = xTaskGetTickCountFromISR();
  TickType_t previous = display_last_refresh_tick;
  if (previous != 0) {
    TickType_t gap = now - previous;
    if (gap > display_max_refresh_gap_ticks) {
      display_max_refresh_gap_ticks = gap;
    }
  }
  display_last_refresh_tick = now;
  ++display_refresh_count;
  BaseType_t should_yield = pdFALSE;
  vTaskNotifyGiveFromISR(static_cast<TaskHandle_t>(user_data), &should_yield);
  return should_yield == pdTRUE;
}

void displayFlush(lv_disp_drv_t *driver, const lv_area_t *, lv_color_t *colors) {
  // In LVGL direct mode every invalid area can invoke flush, but all areas are
  // rendered into the same off-screen framebuffer. Present it only after the
  // final area is complete.
  if (!lv_disp_flush_is_last(driver)) {
    lv_disp_flush_ready(driver);
    return;
  }
  if (!Board::switchFrameBuffer(colors)) {
    Serial.println("LCD frame switch failed");
    Serial.flush();
    delay(20);
    ESP.restart();
    return;
  }
  // Match the panel library's supported double-buffer sequence. Never release
  // the off-screen buffer until a refresh boundary has actually completed.
  ulTaskNotifyValueClear(nullptr, ULONG_MAX);
  TickType_t wait_started = xTaskGetTickCount();
  if (ulTaskNotifyTake(pdTRUE, kFrameBoundaryTimeout) == 0) {
    // A stalled RGB callback must not leave the LVGL task blocked forever.
    // Ask the driver to resynchronise at the next VSYNC, and still keep
    // ownership of the framebuffer until a real boundary arrives.
    Serial.println("LCD frame boundary timeout; requesting RGB resync");
    ++display_recovery_count;
    if (!Board::restartRgbScan()) {
      Serial.println("LCD RGB resync request failed; rebooting");
      Serial.flush();
      delay(20);
      ESP.restart();
      return;
    }
    ulTaskNotifyValueClear(nullptr, ULONG_MAX);
    if (ulTaskNotifyTake(pdTRUE, kFrameBoundaryTimeout) == 0) {
      Serial.println("LCD RGB resync timed out; rebooting");
      Serial.flush();
      delay(20);
      ESP.restart();
      return;
    }
    Serial.println("LCD RGB scan recovered");
    display_full_redraw_requested = true;
  }
  TickType_t wait_ticks = xTaskGetTickCount() - wait_started;
  if (wait_ticks > display_max_flush_wait_ticks) {
    display_max_flush_wait_ticks = wait_ticks;
  }
  lv_disp_flush_ready(driver);
}

void logMemory(const char *stage) {
  Serial.printf(
      "MEMORY,%s,INTERNAL_FREE=%u,INTERNAL_MIN=%u,INTERNAL_LARGEST=%u,"
      "PSRAM_FREE=%u,PSRAM_LARGEST=%u,REFRESH_COUNT=%lu,"
      "REFRESH_MAX_GAP_MS=%lu,FLUSH_MAX_WAIT_MS=%lu,RGB_RECOVERIES=%lu,"
      "FULL_REDRAWS=%lu,MAIN_STACK_FREE=%u\n",
      stage,
      static_cast<unsigned>(
          heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
      static_cast<unsigned>(
          heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL |
                                          MALLOC_CAP_8BIT)),
      static_cast<unsigned>(
          heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL |
                                           MALLOC_CAP_8BIT)),
      static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
      static_cast<unsigned>(
          heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)),
      static_cast<unsigned long>(display_refresh_count),
      static_cast<unsigned long>(
          display_max_refresh_gap_ticks * portTICK_PERIOD_MS),
      static_cast<unsigned long>(
          display_max_flush_wait_ticks * portTICK_PERIOD_MS),
      static_cast<unsigned long>(display_recovery_count),
      static_cast<unsigned long>(display_full_redraw_count),
      static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
}

void touchRead(lv_indev_drv_t *, lv_indev_data_t *data) {
  Board::TouchPoint point{};
  const int count = Board::readTouches(&point, 1);
  if (count > 0) {
    last_touch_point = point;
    touch_release_samples = 0;
    if (!touch_is_pressed) {
      if (++touch_press_samples < kTouchPressConfirmSamples) {
        data->state = LV_INDEV_STATE_REL;
        return;
      }
      touch_is_pressed = true;
      touch_press_samples = 0;
    }
    data->state = LV_INDEV_STATE_PR;
    data->point.x = last_touch_point.x;
    data->point.y = last_touch_point.y;
    return;
  }

  touch_press_samples = 0;
  if (touch_is_pressed &&
      ++touch_release_samples < kTouchReleaseConfirmSamples) {
    data->state = LV_INDEV_STATE_PR;
    data->point.x = last_touch_point.x;
    data->point.y = last_touch_point.y;
    return;
  }

  touch_is_pressed = false;
  touch_release_samples = 0;
  data->state = LV_INDEV_STATE_REL;
}

void tick(void *) {
  lv_tick_inc(2);
}

bool initLvgl() {
  AppUi::init();
  buffer1 = static_cast<lv_color_t *>(Board::frameBuffer(0));
  buffer2 = static_cast<lv_color_t *>(Board::frameBuffer(1));
  if (!buffer1 || !buffer2) {
    Serial.println("LCD double frame buffers unavailable");
    return false;
  }
  if (!Board::attachRefreshFinishCallback(displayRefreshFinished,
                                           xTaskGetCurrentTaskHandle())) {
    Serial.println("LCD VSYNC callback setup failed");
    return false;
  }
  lv_disp_draw_buf_init(&draw_buffer, buffer1, buffer2, kBufferPixels);

  static lv_disp_drv_t display_driver;
  lv_disp_drv_init(&display_driver);
  display_driver.hor_res = kWidth;
  display_driver.ver_res = kHeight;
  display_driver.flush_cb = displayFlush;
  display_driver.draw_buf = &draw_buffer;
  // ESP32_Display_Panel's recommended RGB anti-tearing mode: LVGL redraws and
  // synchronizes only dirty regions between the two full-screen framebuffers.
  // Full refresh rewrote 450 KiB on every slider frame and starved RGB DMA once
  // Wi-Fi/HTTP traffic began competing for the same PSRAM.
  display_driver.direct_mode = 1;
  lv_disp_drv_register(&display_driver);
  AppUi::displayReady();

  static lv_indev_drv_t input_driver;
  lv_indev_drv_init(&input_driver);
  input_driver.type = LV_INDEV_TYPE_POINTER;
  input_driver.read_cb = touchRead;
  input_driver.long_press_time = 800;
  lv_indev_t *input = lv_indev_drv_register(&input_driver);
  if (!input) {
    Serial.println("LVGL touch input registration failed");
    return false;
  }
  lv_timer_set_period(input->driver->read_timer, 8);

  const esp_timer_create_args_t timer_args = {
      .callback = tick,
      .name = "lvgl-tick",
  };
  esp_timer_handle_t timer;
  esp_timer_create(&timer_args, &timer);
  esp_timer_start_periodic(timer, 2000);
  return true;
}

void beginWiFi(const DeviceConfig &config) {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(config.ssid.c_str(), config.password.c_str());
  Serial.printf("Connecting to %s", config.ssid.c_str());
}

bool waitForWiFi() {
  uint32_t started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < 20000) {
    lv_timer_handler();
    delay(20);
    if ((millis() / 500) % 2 == 0) Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("Wi-Fi ready: %s\n", WiFi.localIP().toString().c_str());
    return true;
  }
  return false;
}

void startProvisioning() {
  provisioning = true;
  AppUi::showProvisioning();
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\nAzoria Display Controller");

  if (!Board::begin() || !initLvgl()) {
    Serial.println("Fatal display initialization error");
    return;
  }
  logMemory("display-ready");
  beginUsbProvisioning();

  DeviceConfig config;
  if (!loadDeviceConfig(config)) {
    startProvisioning();
    return;
  }

  saved_config = config;
  have_saved_config = true;
  Kvm::showScreen();
  // Let the Wi-Fi driver reserve its latency-sensitive internal DMA buffers
  // before NimBLE starts. The BLE host is configured to use PSRAM for dynamic
  // allocations, so both radios remain available without starving RGB DMA.
  beginWiFi(saved_config);
  Kvm::startRemote(saved_config);
  remote_started = true;
  if (waitForWiFi()) {
    logMemory("network-ready");
  } else {
    // Keep the saved station credentials and recover automatically if the
    // router was merely unavailable during boot. USB provisioning remains
    // available in parallel if the saved credentials really are wrong.
    Serial.println("Wi-Fi unavailable; retrying saved network in background");
    next_wifi_retry = millis() + 5000;
  }
  next_runtime_diagnostics = millis() + kRuntimeDiagnosticIntervalMs;
}

void loop() {
  bleTransportLoop();
  lv_timer_handler();
  display_full_redraw_pending |= display_full_redraw_requested;
  display_full_redraw_requested = false;
  display_full_redraw_pending |= Kvm::takeFullRedrawRequest();
  usbProvisioningLoop();
  if (have_saved_config) {
    if (WiFi.status() == WL_CONNECTED) {
      next_wifi_retry = millis() + 5000;
    } else if (static_cast<int32_t>(millis() - next_wifi_retry) >= 0) {
      Serial.println("Retrying saved Wi-Fi network");
      WiFi.reconnect();
      next_wifi_retry = millis() + 5000;
    }
  }
  if (!provisioning) {
    Kvm::refresh();
  }
  lv_disp_t *display = lv_disp_get_default();
  lv_disp_draw_buf_t *active_draw_buffer =
      (display != nullptr && display->driver != nullptr)
          ? display->driver->draw_buf
          : nullptr;
  if (display_full_redraw_pending && display != nullptr &&
      active_draw_buffer != nullptr && display->driver->direct_mode &&
      active_draw_buffer->buf1 != nullptr &&
      active_draw_buffer->buf2 != nullptr && !display->rendering_in_progress &&
      !active_draw_buffer->flushing && lv_anim_count_running() == 0) {
    // The first refresh redraws and presents the complete screen. In LVGL
    // direct-mode double buffering that leaves a full-screen sync area for the
    // other buffer, so run the refresh path once more to consume that area.
    // This keeps both framebuffers identical without an unsafe manual memcpy.
    display_full_redraw_pending = false;
    lv_obj_invalidate(lv_disp_get_scr_act(display));
    lv_refr_now(display);
    if (!display->rendering_in_progress && !active_draw_buffer->flushing &&
        lv_anim_count_running() == 0) {
      lv_refr_now(display);
      ++display_full_redraw_count;
    } else {
      display_full_redraw_pending = true;
    }
  }
  if (static_cast<int32_t>(millis() - next_runtime_diagnostics) >= 0) {
    logMemory("runtime");
    next_runtime_diagnostics = millis() + kRuntimeDiagnosticIntervalMs;
  }
  delay(5);
}
