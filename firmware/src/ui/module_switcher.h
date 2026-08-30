#pragma once

#include <lvgl.h>

namespace ModuleSwitcher {

lv_obj_t *createDisplayIcon(lv_obj_t *parent, int x, int y,
                            lv_color_t icon_color);
lv_obj_t *create(lv_obj_t *parent);

}  // namespace ModuleSwitcher
