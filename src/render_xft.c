/**
 * @file render_xft.c
 * @brief Xft renderer backend.
 */

/* See LICENSE for license details. */

#include <math.h>
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
_Static_assert(sizeof(RenderGlyphSpec) == sizeof(XftGlyphFontSpec),
               "RenderGlyphSpec must match XftGlyphFontSpec size");
_Static_assert(offsetof(RenderGlyphSpec, font) ==
                 offsetof(XftGlyphFontSpec, font),
               "RenderGlyphSpec font must match XftGlyphFontSpec");
_Static_assert(offsetof(RenderGlyphSpec, glyph) ==
                 offsetof(XftGlyphFontSpec, glyph),
               "RenderGlyphSpec glyph must match XftGlyphFontSpec");
_Static_assert(offsetof(RenderGlyphSpec, x) == offsetof(XftGlyphFontSpec, x),
               "RenderGlyphSpec x must match XftGlyphFontSpec");
_Static_assert(offsetof(RenderGlyphSpec, y) == offsetof(XftGlyphFontSpec, y),
               "RenderGlyphSpec y must match XftGlyphFontSpec");

extern Display* xft_display(void);
extern Visual* xft_visual(void);
extern Colormap xft_colormap(void);
extern XftDraw* xft_draw_context(void);

static void xft_clear_rect(int x, int y, int width, int height,
                           const RenderColor* color) {
  XftDrawRect(xft_draw_context(), (XftColor*)color, x, y, width, height);
}

static void xft_boxdraw_fill(const RenderColor* color, int x, int y,
                             int width, int height) {
  Display* dpy = xft_display();
  Visual* vis = xft_visual();
  Colormap cmap = xft_colormap();
  XRenderColor xrc = {.red = color->red,
                      .green = color->green,
                      .blue = color->blue,
                      .alpha = color->alpha};
  XftColor xfc;

  XftColorAllocValue(dpy, vis, cmap, &xrc, &xfc);
  xft_clear_rect(x, y, width, height, (const RenderColor*)&xfc);
  XftColorFree(dpy, vis, cmap, &xfc);
}

void render_xft_init(void) { boxdraw_set_fill_rect(xft_boxdraw_fill); }

/* Draws a horizontal dashed line of length `w` starting at `(x, y)`. `wavelen`
 * is the length of the dash plus the length of the gap. `fraction` is the
 * fraction of the dash length compared to `wavelen`. */
static void xft_draw_dashed(int x, int y, int w, int wavelen, float fraction,
                            int thick, const XftColor* color) {
  int dashw = MAX(1, fraction * wavelen);
  for (int i = x - x % wavelen; i < x + w; i += wavelen) {
    int startx = MAX(i, x);
    int endx = MIN(i + dashw, x + w);
    if (startx < endx)
      XftDrawRect(xft_draw_context(), color, startx, y, endx - startx, thick);
  }
}

/* Draws an undercurl. `h` is the total height, including line thickness. */
static void xft_draw_curl(int x, int y, int w, int h, int thick,
                          const XftColor* color) {
  Display* dpy = xft_display();
  XftDraw* draw = xft_draw_context();
  XGCValues gcvals = {.foreground = color->pixel,
                      .line_width = thick,
                      .line_style = LineSolid,
                      .cap_style = CapRound};
  GC gc = XCreateGC(dpy, XftDrawDrawable(draw),
                    GCForeground | GCLineWidth | GCLineStyle | GCCapStyle,
                    &gcvals);

  XRectangle clip = {.x = x, .y = y, .width = w, .height = h};
  XSetClipRectangles(dpy, gc, 0, 0, &clip, 1, Unsorted);

  int yoffset = thick / 2;
  int segh = MAX(1, h - thick);
  /* Make sure every segment is at a 45 degree angle, otherwise it doesn't
   * look good without antialiasing. */
  int segw = segh;
  int wavelen = MAX(1, segw * 2);

  for (int i = x - (x % wavelen); i < x + w; i += wavelen) {
    XPoint points[3] = {{.x = i, .y = y + yoffset},
                        {.x = i + segw, .y = y + yoffset + segh},
                        {.x = i + wavelen, .y = y + yoffset}};
    XDrawLines(dpy, XftDrawDrawable(draw), gc, points, 3, CoordModeOrigin);
  }

  XFreeGC(dpy, gc);
}

static void xft_draw_underline(const RenderColor* color, int x, int y,
                               int width, uint32_t style, int thick,
                               float fthick, int cell_width, int cell_top,
                               int cell_bottom) {
  const XftColor* xftcolor = (const XftColor*)color;
  XftDraw* draw = xft_draw_context();

  (void)cell_top;

  if (style == UNDERLINE_DOUBLE) {
    int doubleh = thick * 2 + ceilf(fthick * 0.5);
    y -= MAX(0, y + doubleh - cell_bottom);
    XftDrawRect(draw, xftcolor, x, y, width, thick);
    XftDrawRect(draw, xftcolor, x, y + doubleh - thick, width, thick);
  } else if (style == UNDERLINE_DOTTED) {
    xft_draw_dashed(x, y, width, thick * 2, 0.5, thick, xftcolor);
  } else if (style == UNDERLINE_DASHED) {
    int wavelen = MAX(2, cell_width * 0.9);
    xft_draw_dashed(x, y, width, wavelen, 0.65, thick, xftcolor);
  } else if (style == UNDERLINE_CURLY) {
    int curlh = thick * 2 + roundf(fthick * 0.75);
    y -= MAX(0, y + curlh - cell_bottom);
    xft_draw_curl(x, y, width, curlh, thick, xftcolor);
  } else {
    XftDrawRect(draw, xftcolor, x, y, width, thick);
  }
}

static void xft_draw_strike(const RenderColor* color, int x, int y, int width,
                            int thick) {
  XftDrawRect(xft_draw_context(), (XftColor*)color, x, y, width, thick);
}

static void xft_draw_glyphs(const RenderGlyphSpec* specs, int len,
                            const RenderColor* color) {
  XftDrawGlyphFontSpec(xft_draw_context(), (const XftColor*)color,
                       (const XftGlyphFontSpec*)specs, len);
}

const RendererOps render_xft_ops = {
  .name = "xft",
  .frame_begin = xstartdraw,
  .frame_end = xfinishdraw,
  .resize = xresize,
  .clear_rect = xft_clear_rect,
  .draw_line = xdrawline,
  .draw_cursor = xdrawcursor,
  .draw_underline = xft_draw_underline,
  .draw_strike = xft_draw_strike,
  .draw_glyphs = xft_draw_glyphs,
  .draw_images = xdrawimages,
  .draw_image_cell = xdrawoneimagecell,
  .images_begin = xstartimagedraw,
  .images_end = xfinishimagedraw,
  .image_upload = 0,
  .image_free = 0,
  .resources_reset = 0,
  .debug_glyph_ppm = 0,
};
