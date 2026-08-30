#pragma once

#include <lvgl.h>

lv_obj_t *createFigmaImage(lv_obj_t *parent, const lv_img_dsc_t *source,
                           int x, int y, lv_color_t recolor,
                           lv_opa_t recolor_opa = LV_OPA_COVER);
