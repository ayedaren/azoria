#include "ui/app_ui.h"

#include <lvgl.h>

#include "ui/assets/app_fonts.h"

namespace AppUi {
namespace {

lv_color_t color(uint32_t value) {
  return lv_color_hex(value);
}

lv_obj_t *label(lv_obj_t *parent, const char *text, int y) {
  lv_obj_t *object = lv_label_create(parent);
  lv_label_set_text(object, text);
  lv_obj_set_style_text_font(object, &azoria_font_zh_16, 0);
  lv_obj_align(object, LV_ALIGN_TOP_MID, 0, y);
  return object;
}

}  // namespace

void init() {
  lv_init();
}

void displayReady() {
  lv_obj_t *screen = lv_scr_act();
  lv_obj_set_style_bg_color(screen, color(0x000000), 0);
  lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
  lv_obj_set_style_text_color(screen, color(0xF8FAFC), 0);
  lv_obj_set_style_text_font(screen, &lv_font_montserrat_18, 0);
}

void showProvisioning() {
  lv_obj_clean(lv_scr_act());
  lv_obj_t *orb = lv_obj_create(lv_scr_act());
  lv_obj_set_size(orb, 128, 128);
  lv_obj_align(orb, LV_ALIGN_TOP_MID, 0, 56);
  lv_obj_set_style_radius(orb, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(orb, color(0x6366F1), 0);
  lv_obj_set_style_border_width(orb, 0, 0);
  lv_obj_set_style_shadow_color(orb, color(0x6366F1), 0);
  lv_obj_set_style_shadow_width(orb, 30, 0);
  lv_obj_set_style_shadow_opa(orb, LV_OPA_40, 0);

  lv_obj_t *usb = lv_label_create(orb);
  lv_label_set_text(usb, LV_SYMBOL_USB);
  lv_obj_set_style_text_font(usb, &lv_font_montserrat_48, 0);
  lv_obj_center(usb);

  label(lv_scr_act(), "USB 配网", 220);
  label(lv_scr_act(), "Mac 保持家庭 Wi-Fi", 280);
  lv_obj_t *waiting = label(lv_scr_act(), "等待 USB 配置", 315);
  lv_obj_set_style_text_color(waiting, color(0xA5B4FC), 0);
  label(lv_scr_act(), "无需创建热点", 365);
  lv_obj_t *hint = label(lv_scr_act(), "配置将安全保存", 420);
  lv_obj_set_style_text_color(hint, color(0x94A3B8), 0);
}

}  // namespace AppUi
