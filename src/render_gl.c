/**
 * @file render_gl.c
 * @brief Optional GLX renderer backend.
 */

/* See LICENSE for license details. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fontconfig/fontconfig.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xft/Xft.h>
#include <GL/gl.h>
#include <GL/glx.h>
#include <GL/glxext.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#include "render.h"
#include "render_gl.h"

extern Display* xft_display(void);
extern XftFont* xft_normal_font(void);

#define GL_ATLAS_SIZE 512
#define GL_ATLAS_PADDING 1
#define GL_ATLAS_ENTRIES 4096

typedef struct {
  int valid;
  XftFont* font;
  unsigned int glyph;
  int pixel_size;
  int style;
  int x, y;
  int width, height;
  int left, top;
} GlGlyphEntry;

static GLuint gl_atlas_texture;
static GlGlyphEntry gl_atlas_entries[GL_ATLAS_ENTRIES];
static int gl_atlas_x = GL_ATLAS_PADDING;
static int gl_atlas_y = GL_ATLAS_PADDING;
static int gl_atlas_row_height;
static int gl_atlas_ready;

typedef void (*gl_swap_interval_ext_fn)(Display*, GLXDrawable, int);
typedef void (*gl_query_drawable_ext_fn)(Display*, GLXDrawable, int,
                                         unsigned int*);
typedef int (*gl_swap_interval_mesa_fn)(unsigned int);

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

static int gl_prepare_glyph(FT_Face face, unsigned int glyph) {
  if (FT_Load_Glyph(face, (FT_UInt)glyph, FT_LOAD_DEFAULT) != 0) return -1;
  if (FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL) != 0) return -1;
  return 0;
}

static void gl_atlas_reset(void) {
  memset(gl_atlas_entries, 0, sizeof(gl_atlas_entries));
  gl_atlas_x = GL_ATLAS_PADDING;
  gl_atlas_y = GL_ATLAS_PADDING;
  gl_atlas_row_height = 0;
}

static void gl_atlas_destroy(void) {
  if (gl_atlas_texture) glDeleteTextures(1, &gl_atlas_texture);
  gl_atlas_texture = 0;
  gl_atlas_ready = 0;
  memset(gl_atlas_entries, 0, sizeof(gl_atlas_entries));
  gl_atlas_x = GL_ATLAS_PADDING;
  gl_atlas_y = GL_ATLAS_PADDING;
  gl_atlas_row_height = 0;
}

static int gl_font_pixel_size(XftFont* font) {
  double size;

  if (!font->pattern) return 0;
  if (FcPatternGetDouble(font->pattern, FC_PIXEL_SIZE, 0, &size) ==
      FcResultMatch)
    return (int)size;
  if (FcPatternGetDouble(font->pattern, FC_SIZE, 0, &size) == FcResultMatch)
    return (int)size;
  return 0;
}

static int gl_font_style(XftFont* font) {
  int slant = FC_SLANT_ROMAN;
  int weight = FC_WEIGHT_NORMAL;

  if (!font->pattern) return 0;
  FcPatternGetInteger(font->pattern, FC_SLANT, 0, &slant);
  FcPatternGetInteger(font->pattern, FC_WEIGHT, 0, &weight);
  return (slant << 16) | (weight & 0xffff);
}

static int gl_atlas_create(void) {
  gl_atlas_destroy();
  glGenTextures(1, &gl_atlas_texture);
  glBindTexture(GL_TEXTURE_2D, gl_atlas_texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, GL_ATLAS_SIZE, GL_ATLAS_SIZE, 0,
               GL_RED, GL_UNSIGNED_BYTE, NULL);
  if (glGetError() != GL_NO_ERROR) {
    gl_atlas_destroy();
    return -1;
  }

  gl_atlas_reset();
  gl_atlas_ready = 1;
  return 0;
}

static int gl_atlas_upload(GlGlyphEntry* entry, const FT_Bitmap* bitmap) {
  size_t pitch =
    (size_t)(bitmap->pitch < 0 ? -bitmap->pitch : bitmap->pitch);
  unsigned int row;

  glBindTexture(GL_TEXTURE_2D, gl_atlas_texture);
  for (row = 0; row < bitmap->rows; row++) {
    const unsigned char* src;
    if (bitmap->pitch < 0)
      src = bitmap->buffer + (bitmap->rows - 1 - row) * pitch;
    else
      src = bitmap->buffer + row * pitch;
    glTexSubImage2D(GL_TEXTURE_2D, 0, entry->x, entry->y + row, entry->width,
                    1, GL_RED, GL_UNSIGNED_BYTE, src);
  }

  return glGetError() == GL_NO_ERROR ? 0 : -1;
}

static GlGlyphEntry* gl_atlas_get(XftFont* font, unsigned int glyph) {
  GlGlyphEntry* entry = NULL;
  FT_Face face;
  FT_Bitmap* bitmap;
  int pixel_size, style, consumed_w, consumed_h;
  int i;

  if (!gl_atlas_ready || !font) return NULL;
  pixel_size = gl_font_pixel_size(font);
  style = gl_font_style(font);

  for (i = 0; i < GL_ATLAS_ENTRIES; i++) {
    GlGlyphEntry* candidate = &gl_atlas_entries[i];
    if (candidate->valid && candidate->font == font &&
        candidate->glyph == glyph && candidate->pixel_size == pixel_size &&
        candidate->style == style)
      return candidate;
    if (!candidate->valid && !entry) entry = candidate;
  }
  if (!entry) {
    gl_atlas_reset();
    entry = &gl_atlas_entries[0];
  }

  face = XftLockFace(font);
  if (!face) return NULL;
  if (gl_prepare_glyph(face, glyph) != 0) {
    XftUnlockFace(font);
    return NULL;
  }

  bitmap = &face->glyph->bitmap;
  consumed_w = (int)bitmap->width + 2 * GL_ATLAS_PADDING;
  consumed_h = (int)bitmap->rows + 2 * GL_ATLAS_PADDING;
  if (consumed_w > GL_ATLAS_SIZE || consumed_h > GL_ATLAS_SIZE) {
    XftUnlockFace(font);
    return NULL;
  }

  if (gl_atlas_y + consumed_h > GL_ATLAS_SIZE) {
    gl_atlas_reset();
    entry = &gl_atlas_entries[0];
  }
  if (gl_atlas_x + consumed_w > GL_ATLAS_SIZE) {
    gl_atlas_x = GL_ATLAS_PADDING;
    gl_atlas_y += gl_atlas_row_height + GL_ATLAS_PADDING;
    gl_atlas_row_height = 0;
  }
  if (gl_atlas_y + consumed_h > GL_ATLAS_SIZE) {
    gl_atlas_reset();
    entry = &gl_atlas_entries[0];
  }

  memset(entry, 0, sizeof(*entry));
  entry->font = font;
  entry->glyph = glyph;
  entry->pixel_size = pixel_size;
  entry->style = style;
  entry->x = gl_atlas_x + GL_ATLAS_PADDING;
  entry->y = gl_atlas_y + GL_ATLAS_PADDING;
  entry->width = (int)bitmap->width;
  entry->height = (int)bitmap->rows;
  entry->left = face->glyph->bitmap_left;
  entry->top = face->glyph->bitmap_top;

  if (gl_atlas_upload(entry, bitmap) != 0) {
    memset(entry, 0, sizeof(*entry));
    gl_atlas_ready = 0;
    XftUnlockFace(font);
    fputs("st: glyph atlas upload failed\n", stderr);
    return NULL;
  }

  entry->valid = 1;
  gl_atlas_x += consumed_w;
  gl_atlas_row_height = MAX(gl_atlas_row_height, consumed_h);
  XftUnlockFace(font);
  return entry;
}

static int gl_write_glyph_ppm(const FT_Bitmap* bitmap, const char* path) {
  size_t pitch = (size_t)(bitmap->pitch < 0 ? -bitmap->pitch : bitmap->pitch);
  FILE* fp = fopen(path, "wb");
  unsigned int y, x;

  if (!fp || bitmap->width == 0 || bitmap->rows == 0) {
    if (fp) fclose(fp);
    return -1;
  }
  if (fprintf(fp, "P6\n%u %u\n255\n", bitmap->width, bitmap->rows) < 0)
    goto fail;

  for (y = 0; y < bitmap->rows; y++) {
    const unsigned char* row = bitmap->buffer + y * pitch;
    for (x = 0; x < bitmap->width; x++) {
      if (fputc(row[x], fp) == EOF || fputc(row[x], fp) == EOF ||
          fputc(row[x], fp) == EOF)
        goto fail;
    }
  }

  if (fclose(fp) != 0) return -1;
  return 0;

fail:
  fclose(fp);
  return -1;
}

static int gl_debug_glyph_ppm(uint32_t codepoint, const char* path) {
  XftFont* font = xft_normal_font();
  FT_UInt glyph;
  FT_Face face;
  int result;

  if (!font || !path || !*path) return -1;
  glyph = XftCharIndex(xft_display(), font, (FcChar32)codepoint);
  if (!glyph) return -1;

  face = XftLockFace(font);
  if (!face) return -1;
  if (gl_prepare_glyph(face, glyph) != 0) {
    XftUnlockFace(font);
    return -1;
  }

  result = gl_write_glyph_ppm(&face->glyph->bitmap, path);
  XftUnlockFace(font);
  return result;
}

static void gl_warn_vsync_unavailable(void) {
  static int warned;

  if (warned) return;
  warned = 1;
  fputs("st: ST_VSYNC=1 requested; no GLX swap-control extension, "
        "continuing unsynced\n",
        stderr);
}

static void gl_setup_vsync(Display* dpy, Window win, int screen) {
  gl_swap_interval_ext_fn swap_interval_ext;
  gl_query_drawable_ext_fn query_drawable_ext;
  gl_swap_interval_mesa_fn swap_interval_mesa;
  const char* extensions;
  const char* value = getenv("ST_VSYNC");
  unsigned int interval = 0;

  if (!value || strcmp(value, "1") != 0) return;
  extensions = glXQueryExtensionsString(dpy, screen);

  swap_interval_ext = (gl_swap_interval_ext_fn)glXGetProcAddress(
      (const GLubyte*)"glXSwapIntervalEXT");
  query_drawable_ext = (gl_query_drawable_ext_fn)glXGetProcAddress(
      (const GLubyte*)"glXQueryDrawable");
  if (extensions && strstr(extensions, "GLX_EXT_swap_control") &&
      swap_interval_ext) {
    swap_interval_ext(dpy, win, 1);
    if (!query_drawable_ext) return;
    query_drawable_ext(dpy, win, GLX_SWAP_INTERVAL_EXT, &interval);
    if (interval > 0) return;
  }

  swap_interval_mesa = (gl_swap_interval_mesa_fn)glXGetProcAddress(
      (const GLubyte*)"glXSwapIntervalMESA");
  if (extensions && strstr(extensions, "GLX_MESA_swap_control") &&
      swap_interval_mesa && swap_interval_mesa(1) == 0)
    return;

  gl_warn_vsync_unavailable();
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

static void gl_resources_reset(void) { gl_atlas_reset(); }

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
    .debug_glyph_ppm = gl_debug_glyph_ppm,
};

static int gl_init_fail(const char** reason, const char* message) {
  if (reason) *reason = message;
  return 0;
}

int gl_init(Display* dpy, Window win, XVisualInfo* vis, const char** reason) {
  int attribs[] = {GLX_DOUBLEBUFFER, True, None};
  GLXFBConfig config = NULL;
  GLXFBConfig* configs;
  int nconfigs = 0;

  if (reason) *reason = NULL;
  if (!dpy || win == None || !vis)
    return gl_init_fail(reason, "no GLX visual");
  if (!glXQueryExtension(dpy, NULL, NULL))
    return gl_init_fail(reason, "no GLX extension");

  configs = glXChooseFBConfig(dpy, vis->screen, attribs, &nconfigs);
  if (!configs || nconfigs == 0)
    return gl_init_fail(reason, "no GLX visual");

  for (int i = 0; i < nconfigs; i++) {
    XVisualInfo* candidate = glXGetVisualFromFBConfig(dpy, configs[i]);
    if (!candidate) continue;
    if (candidate->visualid == vis->visualid) config = configs[i];
    XFree(candidate);
    if (config) break;
  }
  XFree(configs);
  if (!config) return gl_init_fail(reason, "no GLX visual");

  gl_ctx = glXCreateNewContext(dpy, config, GLX_RGBA_TYPE, NULL, True);
  if (!gl_ctx) return gl_init_fail(reason, "no GLX context");

  if (!glXMakeContextCurrent(dpy, win, win, gl_ctx)) {
    glXDestroyContext(dpy, gl_ctx);
    gl_ctx = NULL;
    return gl_init_fail(reason, "no current GLX context");
  }
  gl_dpy = dpy;
  gl_win = win;
  gl_viewport();
  if (gl_atlas_create() != 0)
    fputs("st: glyph atlas allocation failed\n", stderr);
  gl_setup_vsync(dpy, win, vis->screen);
  return 1;
}

void gl_deinit(void) {
  Display* dpy = gl_dpy;
  GLXContext ctx = gl_ctx;

  gl_atlas_destroy();
  gl_dpy = NULL;
  gl_ctx = NULL;
  gl_win = None;
  if (!dpy) return;
  glXMakeContextCurrent(dpy, None, None, NULL);
  if (ctx) glXDestroyContext(dpy, ctx);
}
