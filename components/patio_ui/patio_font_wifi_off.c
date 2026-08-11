/*******************************************************************************
 * Size: 20 px
 * Bpp: 4
 * Opts: --font mdi.ttf --size 20 --bpp 4 --format lvgl --no-compress --force-fast-kern-format -r 0xF05AA -o C:\Users\stesli\code\patio-hub-esphome\components\patio_ui\patio_font_wifi_off.c
 ******************************************************************************/

#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include "lvgl.h"
#else
#include "lvgl.h"
#endif

#ifndef PATIO_FONT_WIFI_OFF
#define PATIO_FONT_WIFI_OFF 1
#endif

#if PATIO_FONT_WIFI_OFF

/*-----------------
 *    BITMAPS
 *----------------*/

/*Store the image of the glyphs*/
static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {
    /* U+F05AA "󰖪" */
    0x9, 0x60, 0x4, 0x8c, 0xdf, 0xfe, 0xc8, 0x40,
    0x0, 0x0, 0xaf, 0x60, 0x7f, 0xff, 0xff, 0xff,
    0xff, 0xe7, 0x0, 0x5, 0xff, 0x60, 0x7c, 0x98,
    0x89, 0xcf, 0xff, 0xfe, 0x40, 0x9f, 0xff, 0x60,
    0x0, 0x0, 0x0, 0x5, 0xcf, 0xf9, 0x0, 0xb5,
    0xbf, 0x60, 0x0, 0x0, 0x0, 0x0, 0x5c, 0x0,
    0x0, 0x0, 0xbf, 0x60, 0x7f, 0xda, 0x50, 0x0,
    0x0, 0x0, 0x0, 0x4d, 0xff, 0x60, 0x7f, 0xff,
    0xe5, 0x0, 0x0, 0x0, 0x9, 0xff, 0xff, 0x60,
    0x7e, 0xff, 0x90, 0x0, 0x0, 0x0, 0xb, 0x60,
    0xbf, 0x60, 0x6, 0xb0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0xaf, 0x60, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x4, 0xbe, 0xff, 0x60, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0xaf, 0xff, 0xff, 0x60,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0xdf, 0xfd,
    0xbf, 0x60, 0x0, 0x0, 0x0, 0x0, 0x0, 0x2,
    0xff, 0x20, 0xbf, 0x60, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x6, 0x50, 0x0, 0xa4, 0x0, 0x0
};


/*---------------------
 *  GLYPH DESCRIPTION
 *--------------------*/

static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {
    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,
    {.bitmap_index = 0, .adv_w = 320, .box_w = 19, .box_h = 15, .ofs_x = 0, .ofs_y = 0}
};

/*---------------------
 *  CHARACTER MAPPING
 *--------------------*/



/*Collect the unicode lists and glyph_id offsets*/
static const lv_font_fmt_txt_cmap_t cmaps[] =
{
    {
        .range_start = 984490, .range_length = 1, .glyph_id_start = 1,
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
    .bitmap_format = 0,
#if LVGL_VERSION_MAJOR == 8
    .cache = &cache
#endif
};



/*-----------------
 *  PUBLIC FONT
 *----------------*/

/*Initialize a public general font descriptor*/
#if LVGL_VERSION_MAJOR >= 8
const lv_font_t patio_font_wifi_off = {
#else
lv_font_t patio_font_wifi_off = {
#endif
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,    /*Function pointer to get glyph's data*/
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,    /*Function pointer to get glyph's bitmap*/
    .line_height = 15,          /*The maximum line height required by the font*/
    .base_line = 0,             /*Baseline measured from the bottom of the line*/
#if !(LVGL_VERSION_MAJOR == 6 && LVGL_VERSION_MINOR == 0)
    .subpx = LV_FONT_SUBPX_NONE,
#endif
#if LV_VERSION_CHECK(7, 4, 0) || LVGL_VERSION_MAJOR >= 8
    .underline_position = 0,
    .underline_thickness = 0,
#endif
    .dsc = &font_dsc,          /*The custom font data. Will be accessed by `get_glyph_bitmap/dsc` */
#if LV_VERSION_CHECK(8, 2, 0) || LVGL_VERSION_MAJOR >= 9
    .fallback = NULL,
#endif
    .user_data = NULL,
};



#endif /*#if PATIO_FONT_WIFI_OFF*/

