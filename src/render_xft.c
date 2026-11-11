/**
 * @file render_xft.c
 * @brief Xft renderer backend.
 */

/* See LICENSE for license details. */

#include "render.h"
#include "win.h"

const RendererOps render_xft_ops = {
  .name = "xft",
  .frame_begin = xstartdraw,
  .frame_end = xfinishdraw,
  .resize = xresize,
  .clear_rect = 0,
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
