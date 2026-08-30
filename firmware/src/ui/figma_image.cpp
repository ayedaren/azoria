#include "ui/figma_image.h"

lv_obj_t *createFigmaImage(lv_obj_t *parent, const lv_img_dsc_t *source,
                           int x, int y, lv_color_t recolor,
                           lv_opa_t recolor_opa) {
  lv_obj_t *image = lv_img_create(parent);
  lv_img_set_src(image, source);
  lv_obj_set_pos(image, x, y);
  lv_obj_set_style_img_recolor(image, recolor, 0);
  lv_obj_set_style_img_recolor_opa(image, recolor_opa, 0);
  lv_obj_clear_flag(image, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  return image;
}
