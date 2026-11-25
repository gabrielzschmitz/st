/**
 * @file render_gl.c
 * @brief Optional GLX renderer backend.
 */

/* See LICENSE for license details. */

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <GL/gl.h>
#include <GL/glx.h>

#include "render.h"
#include "render_gl.h"

static Display* gl_dpy;
static GLXContext gl_ctx;
static Window gl_win;

static void gl_viewport(void) {
  glViewport(0, 0, gl_window_width(), gl_window_height());
}

static void gl_clear_background(void) {
  RenderColor bg = gl_background_color();

  glClearColor(bg.red / 65535.0f, bg.green / 65535.0f, bg.blue / 65535.0f,
               bg.alpha / 65535.0f);
  glClear(GL_COLOR_BUFFER_BIT);
}

/* Until GL drawing lands, the GL renderer presents only the clear color. */
static int gl_frame_begin(void) {
  if (!render_xft_ops.frame_begin()) return 0;
  gl_clear_background();
  return 1;
}

static void gl_frame_end(void) { glXSwapBuffers(gl_dpy, gl_win); }

static void gl_resize(int col, int row) {
  render_xft_ops.resize(col, row);
  gl_viewport();
  gl_clear_background();
  glXSwapBuffers(gl_dpy, gl_win);
}

static void gl_clear_rect(int x, int y, int width, int height,
                          const RenderColor* color) {
  render_xft_ops.clear_rect(x, y, width, height, color);
}

static void gl_draw_line(Line line, int x1, int y1, int x2) {
  render_xft_ops.draw_line(line, x1, y1, x2);
}

static void gl_draw_cursor(int cx, int cy, Glyph g, int ox, int oy, Glyph og,
                           Line line, int len) {
  render_xft_ops.draw_cursor(cx, cy, g, ox, oy, og, line, len);
}

static void gl_draw_underline(const RenderColor* color, int x, int y, int width,
                              uint32_t style, int thick, float fthick,
                              int cell_width, int cell_bottom) {
  render_xft_ops.draw_underline(color, x, y, width, style, thick, fthick,
                                cell_width, cell_bottom);
}

static void gl_draw_strike(const RenderColor* color, int x, int y, int width,
                           int thick) {
  render_xft_ops.draw_strike(color, x, y, width, thick);
}

static void gl_draw_images(Glyph base, Line line, int x1, int y1, int x2) {
  render_xft_ops.draw_images(base, line, x1, y1, x2);
}

static void gl_draw_image_cell(Glyph g, int x, int y) {
  render_xft_ops.draw_image_cell(g, x, y);
}

static void gl_images_begin(int* dirty, int rows) {
  render_xft_ops.images_begin(dirty, rows);
}

static void gl_images_end(void) { render_xft_ops.images_end(); }

static int gl_image_upload(uint32_t image_id, int width, int height,
                           const void* rgba) {
  (void)image_id;
  (void)width;
  (void)height;
  (void)rgba;
  return 0;
}

static void gl_image_free(uint32_t image_id) { (void)image_id; }

static void gl_resources_reset(void) {}

const RendererOps render_gl_ops = {
    .name = "gl",
    .frame_begin = gl_frame_begin,
    .frame_end = gl_frame_end,
    .resize = gl_resize,
    .clear_rect = gl_clear_rect,
    .draw_line = gl_draw_line,
    .draw_cursor = gl_draw_cursor,
    .draw_underline = gl_draw_underline,
    .draw_strike = gl_draw_strike,
    .draw_images = gl_draw_images,
    .draw_image_cell = gl_draw_image_cell,
    .images_begin = gl_images_begin,
    .images_end = gl_images_end,
    .image_upload = gl_image_upload,
    .image_free = gl_image_free,
    .resources_reset = gl_resources_reset,
};

int gl_init(Display* dpy, Window win, XVisualInfo* vis) {
  int attribs[] = {GLX_DOUBLEBUFFER, True, None};
  GLXFBConfig config = NULL;
  GLXFBConfig* configs;
  int nconfigs = 0;

  if (!dpy || win == None || !vis) return 0;
  if (!glXQueryExtension(dpy, NULL, NULL)) return 0;

  configs = glXChooseFBConfig(dpy, vis->screen, attribs, &nconfigs);
  if (!configs) return 0;

  for (int i = 0; i < nconfigs; i++) {
    XVisualInfo* candidate = glXGetVisualFromFBConfig(dpy, configs[i]);
    if (!candidate) continue;
    if (candidate->visualid == vis->visualid) config = configs[i];
    XFree(candidate);
    if (config) break;
  }
  XFree(configs);
  if (!config) return 0;

  gl_ctx = glXCreateNewContext(dpy, config, GLX_RGBA_TYPE, NULL, True);
  if (!gl_ctx) return 0;

  if (!glXMakeContextCurrent(dpy, win, win, gl_ctx)) {
    glXDestroyContext(dpy, gl_ctx);
    gl_ctx = NULL;
    return 0;
  }
  gl_dpy = dpy;
  gl_win = win;
  gl_viewport();
  return 1;
}

void gl_deinit(void) {
  Display* dpy = gl_dpy;
  GLXContext ctx = gl_ctx;

  gl_dpy = NULL;
  gl_ctx = NULL;
  gl_win = None;
  if (!dpy) return;
  glXMakeContextCurrent(dpy, None, None, NULL);
  if (ctx) glXDestroyContext(dpy, ctx);
}
