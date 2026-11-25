/**
 * @file render_xft.c
 * @brief Xft renderer backend.
 */

/* See LICENSE for license details. */

#include <stddef.h>

#include <X11/Xft/Xft.h>

#include "render.h"
#include "win.h"

_Static_assert(sizeof(RenderColor) == sizeof(XftColor),
               "RenderColor must match XftColor size");
_Static_assert(offsetof(RenderColor, pixel) == offsetof(XftColor, pixel),
               "RenderColor pixel must match XftColor");
_Static_assert(offsetof(RenderColor, red) == offsetof(XftColor, color.red),
               "RenderColor red must match XftColor");
_Static_assert(offsetof(RenderColor, green) == offsetof(XftColor, color.green),
               "RenderColor green must match XftColor");
_Static_assert(offsetof(RenderColor, blue) == offsetof(XftColor, color.blue),
               "RenderColor blue must match XftColor");
_Static_assert(offsetof(RenderColor, alpha) == offsetof(XftColor, color.alpha),
               "RenderColor alpha must match XftColor");

extern XftDraw* xft_draw_context(void);

static void xft_clear_rect(int x, int y, int width, int height,
                           const RenderColor* color) {
  XftDrawRect(xft_draw_context(), (XftColor*)color, x, y, width, height);
}

const RendererOps render_xft_ops = {
  .name = "xft",
  .frame_begin = xstartdraw,
  .frame_end = xfinishdraw,
  .resize = xresize,
  .clear_rect = xft_clear_rect,
  .draw_line = xdrawline,
  .draw_cursor = xdrawcursor,
  .draw_underline = 0,
  .draw_strike = 0,
  .draw_images = xdrawimages,
  .draw_image_cell = xdrawoneimagecell,
  .images_begin = xstartimagedraw,
  .images_end = xfinishimagedraw,
  .image_upload = 0,
  .image_free = 0,
  .resources_reset = 0,
};
