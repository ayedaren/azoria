/*******************************************************************************
 * Size: 24 px
 * Bpp: 4
 * Opts: --size 24 --bpp 4 --format lvgl --font /Library/Fonts/SF-Pro-Display-Regular.otf --symbols  --no-kerning --lv-include <lvgl.h> --lv-font-name azoria_font_apple_24 -o src/generated/azoria_font_apple_24.c
 ******************************************************************************/

#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include "lvgl.h"
#else
#include <lvgl.h>
#endif

#ifndef AZORIA_FONT_APPLE_24
#define AZORIA_FONT_APPLE_24 1
#endif

#if AZORIA_FONT_APPLE_24

/*-----------------
 *    BITMAPS
 *----------------*/

/*Store the image of the glyphs*/
static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {
    /* U+F8FF "" */
    0x0, 0xff, 0xe6, 0x3e, 0x88, 0x7, 0xfd, 0x50,
    0x62, 0x1, 0xfe, 0x35, 0x8, 0x0, 0xff, 0x90,
    0x65, 0x80, 0x3e, 0x58, 0x62, 0x6f, 0x39, 0x92,
    0x80, 0x47, 0xb4, 0xf3, 0xbf, 0xd4, 0xcc, 0xab,
    0x0, 0x71, 0x0, 0x61, 0x0, 0xe6, 0x17, 0x20,
    0xf, 0xfb, 0xc7, 0x40, 0x3f, 0xe5, 0x20, 0x70,
    0xf, 0xf8, 0x80, 0x3f, 0xf8, 0x44, 0x0, 0x60,
    0xf, 0xf9, 0x4, 0x30, 0x3, 0xfe, 0x1a, 0x5,
    0x0, 0xff, 0xe0, 0x25, 0x92, 0x80, 0x7f, 0xf0,
    0x34, 0x20, 0x3, 0xfe, 0x26, 0x2, 0x90, 0xf,
    0xf7, 0x80, 0x4c, 0xe0, 0x29, 0x10, 0x30, 0x1b,
    0x20, 0xd, 0x1f, 0xd6, 0xee, 0xcf, 0xe4, 0x0,
    0x0
};


/*---------------------
 *  GLYPH DESCRIPTION
 *--------------------*/

static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {
    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,
    {.bitmap_index = 0, .adv_w = 267, .box_w = 15, .box_h = 20, .ofs_x = 1, .ofs_y = -1}
};

/*---------------------
 *  CHARACTER MAPPING
 *--------------------*/



/*Collect the unicode lists and glyph_id offsets*/
static const lv_font_fmt_txt_cmap_t cmaps[] =
{
    {
        .range_start = 63743, .range_length = 1, .glyph_id_start = 1,
        .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0, .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY
    }
};



/*--------------------
 *  ALL CUSTOM DATA
 *--------------------*/

#if LVGL_VERSION_MAJOR == 8
/*Store all the custom data of the font*/
static  lv_font_fmt_txt_glyph_cache_t cache;
#endif

#if LVGL_VERSION_MAJOR >= 8
static const lv_font_fmt_txt_dsc_t font_dsc = {
#else
static lv_font_fmt_txt_dsc_t font_dsc = {
#endif
    .glyph_bitmap = glyph_bitmap,
    .glyph_dsc = glyph_dsc,
    .cmaps = cmaps,
    .kern_dsc = NULL,
    .kern_scale = 0,
    .cmap_num = 1,
    .bpp = 4,
    .kern_classes = 0,
    .bitmap_format = 1,
#if LVGL_VERSION_MAJOR == 8
    .cache = &cache
#endif
};



/*-----------------
 *  PUBLIC FONT
 *----------------*/

/*Initialize a public general font descriptor*/
#if LVGL_VERSION_MAJOR >= 8
const lv_font_t azoria_font_apple_24 = {
#else
lv_font_t azoria_font_apple_24 = {
#endif
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,    /*Function pointer to get glyph's data*/
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,    /*Function pointer to get glyph's bitmap*/
    .line_height = 20,          /*The maximum line height required by the font*/
    .base_line = 1,             /*Baseline measured from the bottom of the line*/
#if !(LVGL_VERSION_MAJOR == 6 && LVGL_VERSION_MINOR == 0)
    .subpx = LV_FONT_SUBPX_NONE,
#endif
#if LV_VERSION_CHECK(7, 4, 0) || LVGL_VERSION_MAJOR >= 8
    .underline_position = -4,
    .underline_thickness = 1,
#endif
    .dsc = &font_dsc,          /*The custom font data. Will be accessed by `get_glyph_bitmap/dsc` */
#if LV_VERSION_CHECK(8, 2, 0) || LVGL_VERSION_MAJOR >= 9
    .fallback = NULL,
#endif
    .user_data = NULL,
};



#endif /*#if AZORIA_FONT_APPLE_24*/
