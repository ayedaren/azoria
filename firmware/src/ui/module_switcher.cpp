#include "ui/module_switcher.h"

#include "ui/assets/figma_assets.h"
#include "ui/figma_image.h"

namespace ModuleSwitcher {
namespace {

}  // namespace

lv_obj_t *createDisplayIcon(lv_obj_t *parent, int x, int y,
                            lv_color_t icon_color) {
  return createFigmaImage(parent, &figma_tv_active_48, x, y, icon_color);
}

lv_obj_t *create(lv_obj_t *parent) {
  lv_obj_t *switcher = lv_obj_create(parent);
  lv_obj_set_pos(switcher, 16, 60);
  lv_obj_set_size(switcher, 72, 68);
  lv_obj_set_style_bg_opa(switcher, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(switcher, 0, 0);
  lv_obj_set_style_pad_all(switcher, 0, 0);
  lv_obj_set_style_radius(switcher, 0, 0);
  lv_obj_set_style_shadow_width(switcher, 0, 0);
  lv_obj_clear_flag(switcher, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN |
                                LV_OBJ_FLAG_SCROLL_ELASTIC | LV_OBJ_FLAG_SCROLL_MOMENTUM);
  createFigmaImage(switcher, &figma_tv_active_48, 14, 5, lv_color_hex(0xEBF4FF));
  lv_obj_t *underline = lv_obj_create(switcher);
  lv_obj_set_pos(underline, 23, 63);
  lv_obj_set_size(underline, 24, 2);
  lv_obj_set_style_bg_color(underline, lv_color_hex(0x168BFF), 0);
  lv_obj_set_style_bg_opa(underline, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(underline, 0, 0);
  lv_obj_set_style_radius(underline, 1, 0);
  lv_obj_clear_flag(underline, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  return switcher;
}

}  // namespace ModuleSwitcher
