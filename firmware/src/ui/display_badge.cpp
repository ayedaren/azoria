#include "ui/display_badge.h"

#include "ui/assets/icons.h"
#include "ui/image.h"

namespace DisplayBadge {

lv_obj_t *create(lv_obj_t *parent) {
  lv_obj_t *badge = lv_obj_create(parent);
  lv_obj_set_pos(badge, 16, 60);
  lv_obj_set_size(badge, 72, 68);
  lv_obj_set_style_bg_opa(badge, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(badge, 0, 0);
  lv_obj_set_style_pad_all(badge, 0, 0);
  lv_obj_set_style_radius(badge, 0, 0);
  lv_obj_set_style_shadow_width(badge, 0, 0);
  lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN |
                               LV_OBJ_FLAG_SCROLL_ELASTIC | LV_OBJ_FLAG_SCROLL_MOMENTUM);
  createUiImage(badge, &icon_tv_active_48, 14, 5, lv_color_hex(0xEBF4FF));
  lv_obj_t *underline = lv_obj_create(badge);
  lv_obj_set_pos(underline, 23, 63);
  lv_obj_set_size(underline, 24, 2);
  lv_obj_set_style_bg_color(underline, lv_color_hex(0x168BFF), 0);
  lv_obj_set_style_bg_opa(underline, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(underline, 0, 0);
  lv_obj_set_style_radius(underline, 1, 0);
  lv_obj_clear_flag(underline, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  return badge;
}

}  // namespace DisplayBadge
