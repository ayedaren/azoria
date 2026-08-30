#include "modules/kvm/screen.h"

#include <Arduino.h>
#include <WiFi.h>
#include <lvgl.h>
#include <time.h>

#include "modules/kvm/assets/kvm_fonts.h"
#include "modules/kvm/service.h"
#include "ui/assets/app_fonts.h"
#include "ui/assets/figma_assets.h"
#include "ui/figma_image.h"
#include "ui/module_switcher.h"

namespace Kvm {
namespace {

lv_color_t color(uint32_t value) {
  return lv_color_hex(value);
}

lv_obj_t *controls = nullptr;
lv_obj_t *status_dot = nullptr;
lv_obj_t *brightness_slider = nullptr;
lv_obj_t *brightness_value = nullptr;
lv_obj_t *volume_slider = nullptr;
lv_obj_t *volume_value = nullptr;
lv_obj_t *mute_button = nullptr;
lv_obj_t *mute_icon = nullptr;
lv_obj_t *clock_label = nullptr;
lv_obj_t *weekday_label = nullptr;
lv_obj_t *date_label = nullptr;
lv_obj_t *brightness_segments[40]{};
constexpr int kInputCount = 4;
constexpr int kInputOrder[kInputCount] = {3, 1, 2, 0};
lv_obj_t *input_buttons[kInputCount]{};
lv_obj_t *input_icons[kInputCount]{};
lv_obj_t *footer_text = nullptr;
uint32_t shown_revision = UINT32_MAX;
bool local_muted = false;
bool brightness_dragging = false;
bool volume_dragging = false;
uint32_t brightness_last_sent = 0;
uint32_t volume_last_sent = 0;
uint32_t post_interaction_redraw_due = 0;
uint32_t next_clock_update = 0;
uint16_t active_input = 3;
int16_t pending_input = -1;
bool controls_enabled = false;

// LG USB VCP needs roughly 300 ms per preview on this monitor. Producing updates
// faster only keeps Wi-Fi/HTTP continuously busy; the local slider remains
// immediate and release still queues the final value without delay.
constexpr uint32_t kDragSendIntervalMs = 400;
constexpr uint32_t kPostInteractionRedrawDelayMs = 2000;
constexpr int kBrightnessSegmentCount = 40;
constexpr const char *kInputValues[] = {
    "dp1", "hdmi1", "hdmi2", "usbc",
};
constexpr const char *kInputLabels[] = {
    "HDMI1", "HDMI1", "HDMI1", "USB-C",
};

void disableScrolling(lv_obj_t *object) {
  lv_obj_clear_flag(
      object, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN |
                  LV_OBJ_FLAG_SCROLL_ELASTIC | LV_OBJ_FLAG_SCROLL_MOMENTUM);
}

lv_obj_t *label(lv_obj_t *parent, const char *text, int x, int y,
                const lv_font_t *font = &lv_font_montserrat_18) {
  lv_obj_t *object = lv_label_create(parent);
  lv_label_set_text(object, text);
  lv_obj_set_pos(object, x, y);
  lv_obj_set_style_text_font(object, font, 0);
  return object;
}

lv_obj_t *card(lv_obj_t *parent, int x, int y, int width, int height) {
  lv_obj_t *object = lv_obj_create(parent);
  lv_obj_set_pos(object, x, y);
  lv_obj_set_size(object, width, height);
  lv_obj_set_style_bg_color(object, color(0x111827), 0);
  lv_obj_set_style_border_width(object, 0, 0);
  lv_obj_set_style_radius(object, 12, 0);
  lv_obj_set_style_pad_all(object, 0, 0);
  lv_obj_set_style_shadow_width(object, 0, 0);
  disableScrolling(object);
  return object;
}

lv_obj_t *staticLabel(lv_obj_t *parent, const char *text, int x, int y,
                      const lv_font_t *font, uint32_t text_color) {
  lv_obj_t *object = label(parent, text, x, y, font);
  lv_obj_set_style_text_color(object, color(text_color), 0);
  return object;
}

void updateClock() {
  if (!clock_label || !weekday_label || !date_label) return;
  time_t now = time(nullptr);
  struct tm local_time;
  localtime_r(&now, &local_time);
  if (local_time.tm_year < 120) {
    lv_label_set_text(clock_label, "--:--:--");
    lv_label_set_text(weekday_label, "星期--");
    lv_label_set_text(date_label, "----/--/--");
    return;
  }
  char time_text[12];
  char date_text[16];
  snprintf(time_text, sizeof(time_text), "%02d:%02d:%02d", local_time.tm_hour,
           local_time.tm_min, local_time.tm_sec);
  snprintf(date_text, sizeof(date_text), "%04d/%d/%d", local_time.tm_year + 1900,
           local_time.tm_mon + 1, local_time.tm_mday);
  static const char *weekdays[] = {"星期日", "星期一", "星期二", "星期三",
                                   "星期四", "星期五", "星期六"};
  lv_label_set_text(clock_label, time_text);
  lv_label_set_text(weekday_label, weekdays[local_time.tm_wday]);
  lv_label_set_text(date_label, date_text);
}

void updateBrightnessSegments(int value) {
  int active_count = (value * kBrightnessSegmentCount + 99) / 100;
  for (int index = 0; index < kBrightnessSegmentCount; ++index) {
    if (!brightness_segments[index]) continue;
    lv_obj_set_style_bg_color(
        brightness_segments[index],
        color(index < active_count ? 0xE1F4FF : 0x555555), 0);
  }
}

lv_obj_t *createWindowsIcon(lv_obj_t *parent, int x, int y,
                            lv_color_t icon_color) {
  lv_obj_t *root = lv_obj_create(parent);
  lv_obj_set_pos(root, x, y);
  lv_obj_set_size(root, 34, 30);
  lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(root, 0, 0);
  lv_obj_set_style_pad_all(root, 0, 0);
  lv_obj_clear_flag(root, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  const int widths[] = {15, 15, 15, 15};
  const int heights[] = {12, 12, 12, 12};
  const int positions[][2] = {{0, 0}, {18, 0}, {0, 15}, {18, 15}};
  for (int index = 0; index < 4; ++index) {
    lv_obj_t *pane = lv_obj_create(root);
    lv_obj_set_pos(pane, positions[index][0], positions[index][1]);
    lv_obj_set_size(pane, widths[index], heights[index]);
    lv_obj_set_style_bg_color(pane, icon_color, 0);
    lv_obj_set_style_bg_opa(pane, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(pane, 0, 0);
    lv_obj_set_style_radius(pane, 1, 0);
    lv_obj_clear_flag(pane, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  }
  return root;
}

void styleSlider(lv_obj_t *slider, int width, uint32_t accent) {
  lv_obj_set_size(slider, width, 22);
  lv_obj_set_style_bg_color(slider, color(0x000000), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_height(slider, 6, LV_PART_MAIN);
  lv_obj_set_style_radius(slider, 3, LV_PART_MAIN);
  lv_obj_set_style_bg_color(slider, color(accent), LV_PART_INDICATOR);
  lv_obj_set_style_height(slider, 6, LV_PART_INDICATOR);
  lv_obj_set_style_radius(slider, 3, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(slider, color(0xFFFFFF), LV_PART_KNOB);
  lv_obj_set_style_width(slider, 22, LV_PART_KNOB);
  lv_obj_set_style_height(slider, 22, LV_PART_KNOB);
  lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
}

void setFooter(const char *text) {
  if (footer_text) lv_label_set_text(footer_text, text);
}

void scheduleFullRedraw() {
  post_interaction_redraw_due =
      millis() + kPostInteractionRedrawDelayMs;
}

const char *localizedMessage(const char *message) {
  if (!message) return "";
  if (strstr(message, "Saving") || strstr(message, "Saved") ||
      strstr(message, "DDC connected") ||
      strstr(message, "Input command sent") ||
      strstr(message, "Power command sent")) {
    return "";
  }
  if (strstr(message, "Backend offline") ||
      strstr(message, "Wi-Fi offline")) return "离线";
  if (strstr(message, "Finding Mac") ||
      strstr(message, "Connecting")) return "连接中";
  if (strstr(message, "input verification failed")) return "切换失败";
  if (strstr(message, "verification failed")) return "操作失败";
  if (strstr(message, "queue full")) return "请稍后";
  return "";
}

int inputIndex(const char *value) {
  if (!value) return -1;
  for (int index = 0; index < kInputCount; ++index) {
    if (!strcmp(value, kInputValues[index])) return index;
  }
  return -1;
}

void updateInputButtons(bool pending = false) {
  for (int index = 0; index < kInputCount; ++index) {
    if (!input_buttons[index]) continue;
    bool selected = index == active_input;
    bool unavailable = index == 0 || index == 2;
    lv_color_t button_color =
        color(selected ? 0xE1F4FF : unavailable ? 0x131313 : 0x1D1D1D);
    lv_obj_set_style_bg_color(input_buttons[index], button_color, 0);
    lv_obj_set_style_bg_color(input_buttons[index], button_color,
                              LV_STATE_DISABLED);
    lv_obj_set_style_bg_opa(input_buttons[index], LV_OPA_COVER,
                            LV_STATE_DISABLED);
    lv_obj_set_style_border_width(
        input_buttons[index],
        pending && index == pending_input ? 2 : 0, 0);
    lv_obj_set_style_border_width(
        input_buttons[index],
        pending && index == pending_input ? 2 : 0, LV_STATE_DISABLED);
    lv_obj_set_style_border_color(input_buttons[index],
                                  color(0xA5B4FC), 0);

    lv_obj_t *caption = lv_obj_get_child(input_buttons[index],
                                         lv_obj_get_child_cnt(input_buttons[index]) - 1);
    if (caption) {
      lv_obj_set_style_text_color(
          caption, color(selected ? 0x000000 : 0xEBF4FF), 0);
      lv_obj_set_style_text_opa(caption, selected ? LV_OPA_50
                                                   : unavailable ? LV_OPA_30
                                                                 : LV_OPA_50,
                                0);
    }
    lv_obj_t *icon = input_icons[index];
    if (!icon) continue;
    // Every selected source sits on the light Figma tile, so its glyph must
    // switch to the dark optical color. Windows previously stayed light and
    // nearly disappeared against its selected background.
    lv_color_t icon_color = color(selected ? 0x050505 : 0xEBF4FF);
    lv_obj_set_style_img_recolor(icon, icon_color, 0);
    lv_obj_set_style_img_recolor_opa(icon, LV_OPA_COVER, 0);
    lv_obj_set_style_opa(icon, LV_OPA_COVER, LV_STATE_DISABLED);
  }
}

void updateMuteVisual() {
  if (!mute_button || !mute_icon) return;
  const bool muted = local_muted;
  const lv_color_t background = color(muted ? 0x3A1E2B : 0x1D1D1D);
  const lv_color_t foreground = color(muted ? 0xF04438 : 0xEBF4FF);
  lv_obj_set_style_bg_color(mute_button, background, 0);
  lv_obj_set_style_bg_color(mute_button, background, LV_STATE_DISABLED);
  lv_obj_set_style_bg_opa(mute_button, LV_OPA_COVER, LV_STATE_DISABLED);
  lv_obj_set_style_border_width(mute_button, muted ? 1 : 0, 0);
  lv_obj_set_style_border_width(mute_button, muted ? 1 : 0, LV_STATE_DISABLED);
  lv_obj_set_style_border_color(mute_button, color(0xF04438), 0);
  lv_obj_set_style_border_color(mute_button, color(0xF04438), LV_STATE_DISABLED);
  lv_obj_set_style_img_recolor(mute_icon, foreground, 0);
  lv_obj_set_style_img_recolor_opa(mute_icon, LV_OPA_COVER, 0);
}

void brightnessPressed(lv_event_t *) {
  brightness_dragging = true;
  post_interaction_redraw_due = 0;
}

void brightnessChanged(lv_event_t *) {
  int value = lv_slider_get_value(brightness_slider);
  lv_label_set_text_fmt(brightness_value, "%d%%", value);
  updateBrightnessSegments(value);
  if (brightness_dragging &&
      millis() - brightness_last_sent >= kDragSendIntervalMs) {
    if (!queueNumericControl("brightness", value, false)) {
      setFooter("亮度失败");
    }
    brightness_last_sent = millis();
  }
}

void brightnessReleased(lv_event_t *) {
  if (!brightness_dragging) return;
  brightness_dragging = false;
  int value = lv_slider_get_value(brightness_slider);
  if (!queueNumericControl("brightness", value, true)) {
    setFooter("亮度失败");
  }
  brightness_last_sent = millis();
  scheduleFullRedraw();
}

void volumePressed(lv_event_t *) {
  volume_dragging = true;
  post_interaction_redraw_due = 0;
}

void volumeChanged(lv_event_t *) {
  int value = lv_slider_get_value(volume_slider);
  if (volume_value) lv_label_set_text_fmt(volume_value, "%d", value);
  if (volume_dragging &&
      millis() - volume_last_sent >= kDragSendIntervalMs) {
    if (!queueNumericControl("volume", value, false)) {
      setFooter("音量失败");
    }
    volume_last_sent = millis();
  }
}

void volumeReleased(lv_event_t *) {
  if (!volume_dragging) return;
  volume_dragging = false;
  int value = lv_slider_get_value(volume_slider);
  if (!queueNumericControl("volume", value, true)) {
    setFooter("音量失败");
  }
  volume_last_sent = millis();
  scheduleFullRedraw();
}

void muteClicked(lv_event_t *) {
  bool requested = !local_muted;
  if (!queueBooleanControl("mute", requested)) {
    setFooter("静音失败");
    return;
  }
  local_muted = requested;
  updateMuteVisual();
  setFooter("");
  scheduleFullRedraw();
}

void inputClicked(lv_event_t *event) {
  int index = static_cast<int>(
      reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
  if (index < 0 || index >= kInputCount || index == active_input) return;
  if (brightness_dragging || volume_dragging) return;
  if (!queueStringControl("input", kInputValues[index])) {
    setFooter("切换失败");
    return;
  }
  pending_input = static_cast<int16_t>(index);
  updateInputButtons(true);
  setFooter("");
  Serial.printf("INPUT_COMMIT,INDEX=%d,VALUE=%s\n",
                index, kInputValues[index]);
  scheduleFullRedraw();
}

lv_obj_t *createInputButton(lv_obj_t *parent, int index, int slot) {
  lv_obj_t *button = lv_btn_create(parent);
  static const int kInputX[] = {13, 129, 244, 360};
  // The exported 36x36 assets contain different transparent side bearings.
  // Position their visible glyphs on the same optical center, rather than
  // aligning the image frame's top-left corner.
  constexpr int kInputIconX = 39;
  lv_obj_set_pos(button, kInputX[slot], 360);
  lv_obj_set_size(button, 108, 98);
  bool selected = index == active_input;
  bool unavailable = index == 0 || index == 2;
  lv_obj_set_style_bg_color(
      button, color(selected ? 0xE1F4FF : unavailable ? 0x131313 : 0x1D1D1D),
      0);
  lv_obj_set_style_shadow_width(button, 0, 0);
  lv_obj_set_style_radius(button, 12, 0);
  lv_obj_set_style_pad_all(button, 0, 0);
  disableScrolling(button);
  lv_obj_set_ext_click_area(button, 3);
  lv_obj_add_event_cb(
      button, inputClicked, LV_EVENT_CLICKED,
      reinterpret_cast<void *>(static_cast<intptr_t>(index)));

  if (index == 3) {
    input_icons[index] = createFigmaImage(button, &figma_apple_36,
                                          kInputIconX, 22,
                                          color(selected ? 0x050505 : 0xEBF4FF));
  } else if (index == 1) {
    input_icons[index] = createFigmaImage(button, &figma_windows_36,
                                          kInputIconX, 24,
                                          color(selected ? 0x050505 : 0xEBF4FF));
  } else if (index == 0) {
    input_icons[index] = createFigmaImage(button, &figma_link1_36,
                                          kInputIconX, 23,
                                          color(0xEBF4FF));
  } else {
    input_icons[index] = createFigmaImage(button, &figma_link2_36,
                                          kInputIconX, 23,
                                          color(0xEBF4FF));
  }
  lv_obj_t *caption =
      label(button, kInputLabels[index], 0, 65, &lv_font_montserrat_14);
  lv_obj_set_style_text_color(caption, color(selected ? 0x000000 : 0xEBF4FF),
                              0);
  lv_obj_set_style_text_opa(caption, selected ? LV_OPA_50
                                               : unavailable ? LV_OPA_30
                                                             : LV_OPA_50,
                            0);
  lv_obj_set_width(caption, 108);
  lv_obj_set_style_text_align(caption, LV_TEXT_ALIGN_CENTER, 0);
  return button;
}

void setControlsEnabled(bool enabled) {
  if (controls_enabled == enabled) return;
  controls_enabled = enabled;
  lv_obj_t *interactive[] = {
      brightness_slider, volume_slider,
  };
  for (lv_obj_t *object : interactive) {
    if (!object) continue;
    if (enabled) {
      lv_obj_clear_state(object, LV_STATE_DISABLED);
    } else {
      lv_obj_add_state(object, LV_STATE_DISABLED);
    }
  }
  // The input and mute tiles remain visually faithful while the backend is
  // reconnecting. Their handlers still surface a localized failure message.
  if (mute_button) lv_obj_clear_state(mute_button, LV_STATE_DISABLED);
  for (lv_obj_t *button : input_buttons) {
    if (button) lv_obj_clear_state(button, LV_STATE_DISABLED);
  }
}

}  // namespace

void showScreen() {
  lv_obj_clean(lv_scr_act());
  controls = lv_scr_act();
  disableScrolling(controls);
  lv_obj_set_scroll_dir(controls, LV_DIR_NONE);

  createFigmaImage(controls, &figma_azoria_logo_80, 28, 24,
                   color(0xF8FAFC));
  ModuleSwitcher::create(controls);

  status_dot = lv_obj_create(controls);
  lv_obj_set_pos(status_dot, 446, 20);
  lv_obj_set_size(status_dot, 8, 8);
  lv_obj_set_style_radius(status_dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(status_dot, 0, 0);
  lv_obj_set_style_bg_color(status_dot, color(0x70FF00), 0);
  clock_label = label(controls, "--:--:--", 338, 61,
                      &azoria_font_din_condensed_48);
  lv_obj_set_style_text_color(clock_label, color(0xF8FAFC), 0);
  weekday_label = staticLabel(controls, "星期--", 339, 106,
                              &azoria_font_zh_16, 0xF8FAFC);
  date_label = staticLabel(controls, "----/--/--", 411, 109,
                           &azoria_font_din_condensed_16, 0xF8FAFC);
  lv_obj_set_style_text_opa(weekday_label, LV_OPA_70, 0);
  lv_obj_set_style_text_opa(date_label, LV_OPA_70, 0);
  updateClock();

  lv_obj_t *brightness_card = card(controls, 13, 166, 458, 102);
  lv_obj_set_style_bg_color(brightness_card, color(0x1D1D1D), 0);
  createFigmaImage(brightness_card, &figma_sun_16, 18, 27,
                   color(0xEBF4FF));
  lv_obj_t *brightness_title =
      staticLabel(brightness_card, "LIGHT", 41, 24, &lv_font_montserrat_16,
                  0xF8FAFC);
  lv_obj_set_style_text_opa(brightness_title, LV_OPA_50, 0);
  brightness_value = label(brightness_card, "50%", 374, 34,
                            &azoria_font_din_condensed_48);
  lv_obj_set_width(brightness_value, 72);
  lv_obj_set_style_text_align(brightness_value, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_set_style_text_color(brightness_value, color(0xF8FAFC), 0);
  lv_obj_set_style_text_opa(brightness_value, LV_OPA_COVER, 0);
  for (int index = 0; index < kBrightnessSegmentCount; ++index) {
    lv_obj_t *segment = lv_obj_create(brightness_card);
    brightness_segments[index] = segment;
    lv_obj_set_pos(segment, 19 + static_cast<int>(index * 8.85f), 57);
    lv_obj_set_size(segment, 3, 22);
    lv_obj_set_style_bg_color(segment, color(0x555555), 0);
    lv_obj_set_style_bg_opa(segment, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(segment, 0, 0);
    lv_obj_set_style_radius(segment, 2, 0);
    lv_obj_clear_flag(segment, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  }
  brightness_slider = lv_slider_create(brightness_card);
  lv_obj_set_pos(brightness_slider, 15, 48);
  lv_obj_set_size(brightness_slider, 340, 38);
  lv_slider_set_range(brightness_slider, 0, 100);
  lv_slider_set_value(brightness_slider, 50, LV_ANIM_OFF);
  lv_obj_set_style_bg_opa(brightness_slider, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(brightness_slider, LV_OPA_TRANSP, LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(brightness_slider, LV_OPA_TRANSP, LV_PART_KNOB);
  lv_obj_set_style_border_width(brightness_slider, 0, LV_PART_MAIN);
  lv_obj_add_event_cb(
      brightness_slider, brightnessPressed, LV_EVENT_PRESSED, nullptr);
  lv_obj_add_event_cb(
      brightness_slider, brightnessChanged, LV_EVENT_VALUE_CHANGED, nullptr);
  lv_obj_add_event_cb(
      brightness_slider, brightnessReleased, LV_EVENT_RELEASED, nullptr);
  lv_obj_add_event_cb(
      brightness_slider, brightnessReleased, LV_EVENT_PRESS_LOST, nullptr);

  lv_obj_t *volume_card = card(controls, 13, 274, 374, 77);
  lv_obj_set_style_bg_color(volume_card, color(0x1D1D1D), 0);
  createFigmaImage(volume_card, &figma_sound_16, 15, 20,
                   color(0xEBF4FF));
  lv_obj_t *volume_title =
      staticLabel(volume_card, "SOUND", 39, 17, &lv_font_montserrat_16,
                  0xF8FAFC);
  lv_obj_set_style_text_opa(volume_title, LV_OPA_50, 0);
  volume_slider = lv_slider_create(volume_card);
  lv_obj_set_pos(volume_slider, 15, 53);
  lv_slider_set_range(volume_slider, 0, 100);
  lv_slider_set_value(volume_slider, 20, LV_ANIM_OFF);
  styleSlider(volume_slider, 340, 0xA9D2FF);
  lv_obj_add_event_cb(
      volume_slider, volumePressed, LV_EVENT_PRESSED, nullptr);
  lv_obj_add_event_cb(
      volume_slider, volumeChanged, LV_EVENT_VALUE_CHANGED, nullptr);
  lv_obj_add_event_cb(
      volume_slider, volumeReleased, LV_EVENT_RELEASED, nullptr);
  lv_obj_add_event_cb(
      volume_slider, volumeReleased, LV_EVENT_PRESS_LOST, nullptr);

  mute_button = lv_btn_create(controls);
  lv_obj_set_pos(mute_button, 391, 276);
  lv_obj_set_size(mute_button, 79, 77);
  lv_obj_set_style_bg_color(mute_button, color(0x1D1D1D), 0);
  lv_obj_set_style_shadow_width(mute_button, 0, 0);
  lv_obj_set_style_radius(mute_button, 12, 0);
  lv_obj_set_style_pad_all(mute_button, 0, 0);
  disableScrolling(mute_button);
  lv_obj_add_event_cb(mute_button, muteClicked, LV_EVENT_CLICKED, nullptr);
  mute_icon = createFigmaImage(mute_button, &figma_mute_24, 27, 26,
                               color(0xF04438));
  footer_text = staticLabel(controls, "", 0, 0, &azoria_font_zh_16,
                            0xFF6B6B);
  lv_obj_set_width(footer_text, 200);
  lv_obj_set_style_text_align(footer_text, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_align(footer_text, LV_ALIGN_TOP_RIGHT, -16, 132);
  next_clock_update = 0;
  for (int slot = 0; slot < kInputCount; ++slot) {
    int index = kInputOrder[slot];
    input_buttons[index] = createInputButton(controls, index, slot);
    lv_obj_add_state(input_buttons[index], LV_STATE_DISABLED);
  }
  updateInputButtons();
  updateMuteVisual();
  updateBrightnessSegments(50);

  controls_enabled = true;
  setControlsEnabled(false);
}

void refresh() {
  if (!controls || !brightness_slider) return;
  if (static_cast<int32_t>(millis() - next_clock_update) >= 0) {
    updateClock();
    next_clock_update = millis() + 1000;
  }
  RemoteState state = getRemoteState();
  bool brightness_needs_reconcile =
      !brightness_dragging && !state.brightness_pending &&
      lv_slider_get_value(brightness_slider) != state.brightness;
  bool volume_needs_reconcile =
      !volume_dragging && !state.volume_pending &&
      lv_slider_get_value(volume_slider) != state.volume;
  bool mute_needs_reconcile =
      !state.mute_pending && local_muted != state.muted;
  int reported_input = inputIndex(state.input);
  bool input_needs_reconcile =
      !state.input_pending && reported_input >= 0 &&
      active_input != static_cast<uint16_t>(reported_input);
  bool enabled_needs_reconcile = controls_enabled != state.ready;
  if (state.revision == shown_revision &&
      !brightness_needs_reconcile && !volume_needs_reconcile &&
      !mute_needs_reconcile &&
      !input_needs_reconcile &&
      !enabled_needs_reconcile) {
    return;
  }
  shown_revision = state.revision;
  setControlsEnabled(state.ready);

  lv_obj_set_style_bg_color(
      status_dot,
      color(!state.ready ? 0xF59E0B :
            state.online ? 0x70FF00 : 0xEF4444),
      0);

  if (brightness_needs_reconcile) {
    lv_slider_set_value(
        brightness_slider, state.brightness, LV_ANIM_OFF);
    lv_label_set_text_fmt(brightness_value, "%d%%", state.brightness);
    updateBrightnessSegments(state.brightness);
  }
  if (volume_needs_reconcile) {
    lv_slider_set_value(volume_slider, state.volume, LV_ANIM_OFF);
    if (volume_value) lv_label_set_text_fmt(volume_value, "%d", state.volume);
  }
  if (mute_needs_reconcile) {
    local_muted = state.muted;
    updateMuteVisual();
  }
  if (input_needs_reconcile) {
    active_input = static_cast<uint16_t>(reported_input);
  }
  if (!state.input_pending) {
    pending_input = -1;
  }
  updateInputButtons(state.input_pending);

  const char *message = localizedMessage(state.message);
  if (footer_text && strcmp(lv_label_get_text(footer_text), message)) {
    lv_label_set_text(footer_text, message);
    lv_obj_align(footer_text, LV_ALIGN_TOP_RIGHT, -16, 132);
  }
}

bool takeFullRedrawRequest() {
  if (post_interaction_redraw_due == 0 ||
      brightness_dragging || volume_dragging ||
      static_cast<int32_t>(millis() - post_interaction_redraw_due) < 0) {
    return false;
  }
  post_interaction_redraw_due = 0;
  return true;
}

}  // namespace Kvm
