/**
 * @file render_gl.c
 * @brief Optional GLX renderer backend.
 */

/* See LICENSE for license details. */

#define GL_GLEXT_PROTOTYPES

#include <math.h>
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
#include "trace.h"

extern Display* xft_display(void);
extern XftFont* xft_normal_font(void);
extern Visual* xft_visual(void);
extern Colormap xft_colormap(void);

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

/* Defined below with the row batch cache. */
static void gl_rows_invalidate_all(void);
static int gl_has_drawables(void);

#define GL_QUAD_FLOATS 8

static const char gl_vertex_shader[] =
    "#version 120\n"
    "attribute vec2 a_position;\n"
    "attribute vec2 a_texcoord;\n"
    "attribute vec4 a_color;\n"
    "uniform vec2 u_viewport;\n"
    "varying vec2 v_texcoord;\n"
    "varying vec4 v_color;\n"
    "void main(void) {\n"
    "  vec2 zeroToOne = a_position / u_viewport;\n"
    "  vec2 clip = zeroToOne * 2.0 - 1.0;\n"
    "  gl_Position = vec4(clip.x, -clip.y, 0.0, 1.0);\n"
    "  v_texcoord = a_texcoord;\n"
    "  v_color = a_color;\n"
    "}\n";

static const char gl_fragment_shader[] =
    "#version 120\n"
    "uniform sampler2D u_atlas;\n"
    "varying vec2 v_texcoord;\n"
    "varying vec4 v_color;\n"
    "void main(void) {\n"
    "  if (v_texcoord.x < 0.0) {\n"
    "    gl_FragColor = v_color;\n"
    "  } else {\n"
    "    float alpha = texture2D(u_atlas, v_texcoord).r;\n"
    "    gl_FragColor = vec4(v_color.rgb, v_color.a * alpha);\n"
    "  }\n"
    "}\n";

static GLuint gl_text_program;
static GLuint gl_text_vbo;
static GLint gl_attr_position = -1;
static GLint gl_attr_texcoord = -1;
static GLint gl_attr_color = -1;
static GLint gl_uniform_viewport = -1;
static GLint gl_uniform_atlas = -1;
static float* gl_quad_data;
static size_t gl_quad_capacity;
static size_t gl_quad_count;
static float* gl_upload_scratch;
static size_t gl_upload_scratch_capacity;
static size_t gl_vbo_capacity;
static size_t gl_vbo_row_total;
static size_t gl_vbo_dynamic_start;
static size_t gl_vbo_dynamic_reserve = 4096;
static int gl_text_ready;
static int gl_eviction_logged;

typedef struct {
  RenderColor color;
  RenderGlyphSpec* specs;
  int len;
} GlFallbackBatch;

typedef struct {
  GlFallbackBatch* batches;
  size_t count;
  size_t capacity;
  int used;
} GlFallbackList;

typedef struct {
  float* data;
  size_t count;
  size_t capacity;
  size_t start;
  GlFallbackList fallback;
  int valid;
  int upload_pending;
} GlRowBatch;

static GlFallbackList gl_fallback_active;
static XftDraw* gl_fallback_draw;
static int gl_fallback_warned;

static GlRowBatch* gl_row_batches;
static size_t gl_row_count;
static int* gl_frame_dirty;
static size_t gl_frame_dirty_count;
static unsigned char* gl_row_submitted;
static size_t gl_row_submitted_count;
static int gl_build_row = -1;
static int gl_full_rebuild;

typedef void (*gl_swap_interval_ext_fn)(Display*, GLXDrawable, int);
typedef void (*gl_query_drawable_ext_fn)(Display*, GLXDrawable, int,
                                         unsigned int*);
typedef int (*gl_swap_interval_mesa_fn)(unsigned int);
typedef void (*gl_copy_sub_buffer_mesa_fn)(Display*, GLXDrawable, int, int,
                                           int, int);

static Display* gl_dpy;
static GLXContext gl_ctx;
static Window gl_win;
static gl_copy_sub_buffer_mesa_fn gl_copy_sub_buffer;

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

static void gl_atlas_evict(void) {
  gl_atlas_reset();
  gl_rows_invalidate_all();
  trace_add_atlas_evictions(1);
  if (!gl_eviction_logged) {
    fputs("st: glyph atlas full; evicting cache\n", stderr);
    gl_eviction_logged = 1;
  }
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
  /* Bold and italic XftFont variants have distinct face pointers, but keeping
   * pattern style in the key also covers reused faces with changed settings. */
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

  /* The face pointer is part of the key. Fallback faces from frc[] therefore
   * cannot collide with the primary face or with each other, even when they
   * assign the same glyph index. */
  for (i = 0; i < GL_ATLAS_ENTRIES; i++) {
    GlGlyphEntry* candidate = &gl_atlas_entries[i];
    if (candidate->valid && candidate->font == font &&
        candidate->glyph == glyph && candidate->pixel_size == pixel_size &&
        candidate->style == style)
      return candidate;
    if (!candidate->valid && !entry) entry = candidate;
  }
  if (!entry) {
    gl_atlas_evict();
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
    gl_atlas_evict();
    entry = &gl_atlas_entries[0];
  }
  if (gl_atlas_x + consumed_w > GL_ATLAS_SIZE) {
    gl_atlas_x = GL_ATLAS_PADDING;
    gl_atlas_y += gl_atlas_row_height + GL_ATLAS_PADDING;
    gl_atlas_row_height = 0;
  }
  if (gl_atlas_y + consumed_h > GL_ATLAS_SIZE) {
    gl_atlas_evict();
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

static GLuint gl_compile_shader(GLenum type, const char* source) {
  GLuint shader = glCreateShader(type);
  GLint compiled;
  char log[512];

  glShaderSource(shader, 1, &source, NULL);
  glCompileShader(shader);
  glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
  if (!compiled) {
    log[0] = '\0';
    glGetShaderInfoLog(shader, sizeof(log), NULL, log);
    fprintf(stderr, "st: glyph shader compile failed: %s\n", log);
    glDeleteShader(shader);
    return 0;
  }
  return shader;
}

static void gl_text_destroy(void) {
  if (gl_text_program) glDeleteProgram(gl_text_program);
  if (gl_text_vbo) glDeleteBuffers(1, &gl_text_vbo);
  gl_text_program = 0;
  gl_text_vbo = 0;
  gl_attr_position = -1;
  gl_attr_texcoord = -1;
  gl_attr_color = -1;
  gl_uniform_viewport = -1;
  gl_uniform_atlas = -1;
  free(gl_upload_scratch);
  gl_upload_scratch = NULL;
  gl_upload_scratch_capacity = 0;
  free(gl_quad_data);
  gl_quad_data = NULL;
  gl_quad_capacity = 0;
  gl_quad_count = 0;
  gl_vbo_capacity = 0;
  gl_vbo_row_total = 0;
  gl_vbo_dynamic_start = 0;
  gl_text_ready = 0;
}

static int gl_text_create(void) {
  GLuint vertex, fragment, program;
  GLint linked;
  char log[512];

  gl_text_destroy();
  vertex = gl_compile_shader(GL_VERTEX_SHADER, gl_vertex_shader);
  if (!vertex) return -1;
  fragment = gl_compile_shader(GL_FRAGMENT_SHADER, gl_fragment_shader);
  if (!fragment) {
    glDeleteShader(vertex);
    return -1;
  }

  program = glCreateProgram();
  glAttachShader(program, vertex);
  glAttachShader(program, fragment);
  glLinkProgram(program);
  glDeleteShader(vertex);
  glDeleteShader(fragment);
  glGetProgramiv(program, GL_LINK_STATUS, &linked);
  if (!linked) {
    log[0] = '\0';
    glGetProgramInfoLog(program, sizeof(log), NULL, log);
    fprintf(stderr, "st: glyph shader link failed: %s\n", log);
    glDeleteProgram(program);
    return -1;
  }

  gl_text_program = program;
  gl_attr_position = glGetAttribLocation(program, "a_position");
  gl_attr_texcoord = glGetAttribLocation(program, "a_texcoord");
  gl_attr_color = glGetAttribLocation(program, "a_color");
  gl_uniform_viewport = glGetUniformLocation(program, "u_viewport");
  gl_uniform_atlas = glGetUniformLocation(program, "u_atlas");
  if (gl_attr_position < 0 || gl_attr_texcoord < 0 || gl_attr_color < 0 ||
      gl_uniform_viewport < 0 || gl_uniform_atlas < 0) {
    gl_text_destroy();
    return -1;
  }

  glGenBuffers(1, &gl_text_vbo);
  if (!gl_text_vbo || glGetError() != GL_NO_ERROR) {
    gl_text_destroy();
    return -1;
  }

  gl_text_ready = 1;
  return 0;
}

static int gl_quads_reserve(size_t floats) {
  size_t needed = gl_quad_count + floats;
  size_t capacity = gl_quad_capacity ? gl_quad_capacity : 4096;
  float* data;

  if (needed <= gl_quad_capacity) return 0;
  while (capacity < needed) capacity *= 2;
  data = realloc(gl_quad_data, capacity * sizeof(float));
  if (!data) return -1;
  gl_quad_data = data;
  gl_quad_capacity = capacity;
  return 0;
}

static void gl_append_vertex(float* p, float x, float y, float u, float v,
                             const float color[4]) {
  p[0] = x;
  p[1] = y;
  p[2] = u;
  p[3] = v;
  p[4] = color[0];
  p[5] = color[1];
  p[6] = color[2];
  p[7] = color[3];
}

static void gl_fallback_warn(void) {
  if (gl_fallback_warned) return;
  gl_fallback_warned = 1;
  fputs("st: glyph atlas unavailable; using Xft text\n", stderr);
}

static int gl_fallback_append_batch(GlFallbackList* list,
                                    const GlFallbackBatch* source) {
  GlFallbackBatch* batches;
  GlFallbackBatch* batch;
  RenderGlyphSpec* copy;
  size_t capacity;

  if (source->len <= 0 || !source->specs) return 0;
  if (list->count == list->capacity) {
    capacity = list->capacity ? list->capacity * 2 : 16;
    batches = realloc(list->batches, capacity * sizeof(*batches));
    if (!batches) return -1;
    list->batches = batches;
    list->capacity = capacity;
  }
  copy = malloc((size_t)source->len * sizeof(*copy));
  if (!copy) return -1;
  memcpy(copy, source->specs, (size_t)source->len * sizeof(*copy));
  batch = &list->batches[list->count++];
  batch->color = source->color;
  batch->specs = copy;
  batch->len = source->len;
  list->used = 1;
  return 0;
}

static int gl_fallback_append(GlFallbackList* list,
                              const RenderGlyphSpec* specs, int len,
                              const RenderColor* color) {
  GlFallbackBatch batch;

  if (len <= 0) return 0;
  batch.color = *color;
  batch.specs = (RenderGlyphSpec*)specs;
  batch.len = len;
  return gl_fallback_append_batch(list, &batch);
}

static void gl_fallback_list_clear(GlFallbackList* list) {
  size_t i;

  for (i = 0; i < list->count; i++) free(list->batches[i].specs);
  list->count = 0;
  list->used = 0;
}

static void gl_fallback_append_list(GlFallbackList* target,
                                    const GlFallbackList* source) {
  size_t i;

  for (i = 0; i < source->count; i++)
    gl_fallback_append_batch(target, &source->batches[i]);
}

static void gl_fallback_replay(GlFallbackList* list) {
  size_t i;

  if (!list->used) return;
  if (!gl_fallback_draw) {
    gl_fallback_draw =
      XftDrawCreate(gl_dpy, gl_win, xft_visual(), xft_colormap());
    if (!gl_fallback_draw) {
      if (!gl_fallback_warned) {
        fputs("st: Xft text fallback unavailable\n", stderr);
        gl_fallback_warned = 1;
      }
      gl_fallback_list_clear(list);
      return;
    }
  }
  for (i = 0; i < list->count; i++) {
    GlFallbackBatch* batch = &list->batches[i];
    XftDrawGlyphFontSpec(gl_fallback_draw, (const XftColor*)&batch->color,
                         (const XftGlyphFontSpec*)batch->specs, batch->len);
  }
  gl_fallback_list_clear(list);
}

static int gl_rows_reserve(size_t rows) {
  GlRowBatch* batches;
  size_t i;

  if (rows <= gl_row_count) return 0;
  batches = realloc(gl_row_batches, rows * sizeof(*batches));
  if (!batches) return -1;
  memset(batches + gl_row_count, 0,
         (rows - gl_row_count) * sizeof(*batches));
  gl_row_batches = batches;
  gl_row_count = rows;
  return 0;
}

static void gl_row_clear(GlRowBatch* row) {
  gl_fallback_list_clear(&row->fallback);
  row->count = 0;
  row->valid = 0;
  row->upload_pending = 0;
}

static int gl_row_set(GlRowBatch* row, const float* data, size_t count) {
  float* buffer;

  if (count == 0) {
    row->count = 0;
    row->valid = 1;
    return 0;
  }
  if (count > row->capacity) {
    size_t capacity = row->capacity ? row->capacity : 512;
    while (capacity < count) capacity *= 2;
    buffer = realloc(row->data, capacity * sizeof(*buffer));
    if (!buffer) return -1;
    row->data = buffer;
    row->capacity = capacity;
  }
  memcpy(row->data, data, count * sizeof(*data));
  row->count = count;
  row->valid = 1;
  return 0;
}

static int gl_vbo_layout(void) {
  size_t i, total = 0;

  if (!gl_text_ready || !gl_text_vbo) return -1;
  for (i = 0; i < gl_row_count; i++) {
    GlRowBatch* row = &gl_row_batches[i];
    if (!row->valid || !row->count) continue;
    row->start = total;
    total += row->count;
  }
  gl_vbo_row_total = total;
  gl_vbo_dynamic_start = total;
  total += gl_vbo_dynamic_reserve;

  glBindBuffer(GL_ARRAY_BUFFER, gl_text_vbo);
  glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(total * sizeof(float)), NULL,
               GL_DYNAMIC_DRAW);
  if (glGetError() != GL_NO_ERROR) {
    gl_vbo_capacity = 0;
    return -1;
  }
  gl_vbo_capacity = total;
  for (i = 0; i < gl_row_count; i++) {
    GlRowBatch* row = &gl_row_batches[i];
    if (!row->valid || !row->count) continue;
    glBufferSubData(GL_ARRAY_BUFFER, (GLintptr)(row->start * sizeof(float)),
                    (GLsizeiptr)(row->count * sizeof(float)), row->data);
    row->upload_pending = 0;
  }
  return glGetError() == GL_NO_ERROR ? 0 : -1;
}

static int gl_vbo_upload_pending(void) {
  size_t i, offset = 0, upload_start = 0;
  int relayout = 0;
  int has_pending = 0;

  for (i = 0; i < gl_row_count; i++) {
    GlRowBatch* row = &gl_row_batches[i];
    if (!row->valid || !row->count) continue;
    if (row->start != offset) relayout = 1;
    if (row->upload_pending && !has_pending) upload_start = offset;
    if (row->upload_pending) has_pending = 1;
    offset += row->count;
  }
  if (offset != gl_vbo_row_total) relayout = 1;
  if (relayout) return gl_vbo_layout();
  if (!has_pending) return 0;
  if (!gl_vbo_capacity || !gl_text_vbo) return -1;
  if (offset > gl_upload_scratch_capacity) {
    float* resized = realloc(gl_upload_scratch, offset * sizeof(*resized));
    if (!resized) return -1;
    gl_upload_scratch = resized;
    gl_upload_scratch_capacity = offset;
  }

  for (i = 0; i < gl_row_count; i++) {
    GlRowBatch* row = &gl_row_batches[i];
    if (!row->valid || !row->count) continue;
    memcpy(gl_upload_scratch + row->start, row->data,
           row->count * sizeof(*gl_upload_scratch));
    row->upload_pending = 0;
  }
  glBindBuffer(GL_ARRAY_BUFFER, gl_text_vbo);
  glBufferSubData(GL_ARRAY_BUFFER, (GLintptr)(upload_start * sizeof(float)),
                  (GLsizeiptr)((offset - upload_start) * sizeof(float)),
                  gl_upload_scratch + upload_start);
  return glGetError() == GL_NO_ERROR ? 0 : -1;
}

static void gl_rows_invalidate_all(void) {
  size_t i;

  for (i = 0; i < gl_row_count; i++) gl_row_clear(&gl_row_batches[i]);
  gl_full_rebuild = 1;
}

static int gl_frame_arrays_reserve(size_t rows) {
  int* dirty;
  unsigned char* submitted;

  if (gl_rows_reserve(rows) != 0) return -1;
  if (rows > gl_frame_dirty_count) {
    dirty = realloc(gl_frame_dirty, rows * sizeof(*dirty));
    if (!dirty) return -1;
    memset(dirty + gl_frame_dirty_count, 0,
           (rows - gl_frame_dirty_count) * sizeof(*dirty));
    gl_frame_dirty = dirty;
    gl_frame_dirty_count = rows;
  }
  if (rows > gl_row_submitted_count) {
    submitted = realloc(gl_row_submitted, rows * sizeof(*submitted));
    if (!submitted) return -1;
    memset(submitted + gl_row_submitted_count, 0,
           (rows - gl_row_submitted_count) * sizeof(*submitted));
    gl_row_submitted = submitted;
    gl_row_submitted_count = rows;
  }
  return 0;
}

static void gl_row_replay(GlRowBatch* row) {
  if (!row->valid) return;
  gl_fallback_append_list(&gl_fallback_active, &row->fallback);
}

static void gl_append_rect(int x, int y, int width, int height,
                           const RenderColor* color) {
  float rgba[4];
  float x0, y0, x1, y1;
  float* p;

  if (width <= 0 || height <= 0 || !color) return;
  if (gl_quads_reserve(6 * GL_QUAD_FLOATS) != 0) return;

  rgba[0] = color->red / 65535.0f;
  rgba[1] = color->green / 65535.0f;
  rgba[2] = color->blue / 65535.0f;
  rgba[3] = color->alpha / 65535.0f;
  x0 = (float)x;
  y0 = (float)y;
  x1 = (float)(x + width);
  y1 = (float)(y + height);

  p = gl_quad_data + gl_quad_count;
  gl_append_vertex(p, x0, y0, -1.0f, -1.0f, rgba);
  gl_append_vertex(p + GL_QUAD_FLOATS, x0, y1, -1.0f, -1.0f, rgba);
  gl_append_vertex(p + 2 * GL_QUAD_FLOATS, x1, y0, -1.0f, -1.0f, rgba);
  gl_append_vertex(p + 3 * GL_QUAD_FLOATS, x1, y0, -1.0f, -1.0f, rgba);
  gl_append_vertex(p + 4 * GL_QUAD_FLOATS, x0, y1, -1.0f, -1.0f, rgba);
  gl_append_vertex(p + 5 * GL_QUAD_FLOATS, x1, y1, -1.0f, -1.0f, rgba);
  gl_quad_count += 6 * GL_QUAD_FLOATS;
}

static void gl_draw_glyphs(const RenderGlyphSpec* specs, int len,
                           const RenderColor* color) {
  float rgba[4] = {color->red / 65535.0f, color->green / 65535.0f,
                   color->blue / 65535.0f, color->alpha / 65535.0f};
  int i;

  if (!gl_text_ready || !gl_atlas_ready) {
    if (len > 0) {
      gl_fallback_warn();
      if (gl_build_row >= 0 &&
          gl_fallback_append(&gl_row_batches[gl_build_row].fallback, specs, len,
                             color) != 0 &&
          gl_row_batches[gl_build_row].valid)
        gl_row_batches[gl_build_row].valid = 0;
      gl_fallback_append(&gl_fallback_active, specs, len, color);
    }
    return;
  }
  if (len <= 0) return;
  if (gl_quads_reserve((size_t)len * 6 * GL_QUAD_FLOATS) != 0) return;
  for (i = 0; i < len; i++) {
    GlGlyphEntry* entry;
    float x0, y0, x1, y1, u0, v0, u1, v1;
    float* p;

    if (!specs[i].font) continue;
    entry = gl_atlas_get((XftFont*)specs[i].font, specs[i].glyph);
    if (!entry) {
      /* gl_atlas_get disables the atlas when an upload fails. Recover the
       * current spec through Xft instead of dropping it. */
      if (!gl_atlas_ready || !gl_text_ready) {
        gl_fallback_warn();
        if (gl_build_row >= 0)
          gl_fallback_append(&gl_row_batches[gl_build_row].fallback, &specs[i],
                             1, color);
        gl_fallback_append(&gl_fallback_active, &specs[i], 1, color);
      }
      continue;
    }
    if (entry->width <= 0 || entry->height <= 0) continue;

    /* xmakeglyphfontspecs resolves ATTR_WIDE advances and HarfBuzz combining
     * offsets into spec.x/y. Use those values directly so GL placement matches
     * Xft for wide cells and combining marks. */
    x0 = specs[i].x + entry->left;
    y0 = specs[i].y - entry->top;
    x1 = x0 + entry->width;
    y1 = y0 + entry->height;
    u0 = (entry->x + 0.5f) / GL_ATLAS_SIZE;
    v0 = (entry->y + 0.5f) / GL_ATLAS_SIZE;
    u1 = (entry->x + entry->width - 0.5f) / GL_ATLAS_SIZE;
    v1 = (entry->y + entry->height - 0.5f) / GL_ATLAS_SIZE;

    p = gl_quad_data + gl_quad_count;
    gl_append_vertex(p, x0, y0, u0, v0, rgba);
    gl_append_vertex(p + GL_QUAD_FLOATS, x0, y1, u0, v1, rgba);
    gl_append_vertex(p + 2 * GL_QUAD_FLOATS, x1, y0, u1, v0, rgba);
    gl_append_vertex(p + 3 * GL_QUAD_FLOATS, x1, y0, u1, v0, rgba);
    gl_append_vertex(p + 4 * GL_QUAD_FLOATS, x0, y1, u0, v1, rgba);
    gl_append_vertex(p + 5 * GL_QUAD_FLOATS, x1, y1, u1, v1, rgba);
    gl_quad_count += 6 * GL_QUAD_FLOATS;
  }
}

static void gl_flush_glyphs(void) {
  size_t stride = GL_QUAD_FLOATS * sizeof(float);
  void* offset_color = (void*)(4 * sizeof(float));
  size_t total;

  if (!gl_text_ready || !gl_has_drawables()) return;
  if (gl_quad_count > gl_vbo_capacity - gl_vbo_dynamic_start) {
    size_t reserve = gl_vbo_dynamic_reserve ? gl_vbo_dynamic_reserve : 4096;
    while (reserve < gl_quad_count) reserve *= 2;
    gl_vbo_dynamic_reserve = reserve;
    if (gl_vbo_layout() != 0) return;
  } else if (gl_vbo_upload_pending() != 0) {
    return;
  }
  if (!gl_vbo_capacity) return;

  total = gl_vbo_row_total + gl_quad_count;
  glUseProgram(gl_text_program);
  glBindBuffer(GL_ARRAY_BUFFER, gl_text_vbo);
  if (gl_quad_count) {
    glBufferSubData(GL_ARRAY_BUFFER,
                    (GLintptr)(gl_vbo_dynamic_start * sizeof(float)),
                    (GLsizeiptr)(gl_quad_count * sizeof(float)), gl_quad_data);
  }
  glEnableVertexAttribArray(gl_attr_position);
  glEnableVertexAttribArray(gl_attr_texcoord);
  glEnableVertexAttribArray(gl_attr_color);
  glVertexAttribPointer(gl_attr_position, 2, GL_FLOAT, GL_FALSE, stride, NULL);
  glVertexAttribPointer(gl_attr_texcoord, 2, GL_FLOAT, GL_FALSE, stride,
                        (void*)(2 * sizeof(float)));
  glVertexAttribPointer(gl_attr_color, 4, GL_FLOAT, GL_FALSE, stride,
                        offset_color);

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, gl_atlas_texture);
  glUniform1i(gl_uniform_atlas, 0);
  glUniform2f(gl_uniform_viewport, (float)gl_window_width(),
              (float)gl_window_height());
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glDisable(GL_DEPTH_TEST);
  glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(total / GL_QUAD_FLOATS));
  glDisableVertexAttribArray(gl_attr_position);
  glDisableVertexAttribArray(gl_attr_texcoord);
  glDisableVertexAttribArray(gl_attr_color);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glUseProgram(0);

  gl_quad_count = 0;
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

static int gl_frame_begin(void) {
  if (!render_xft_ops.frame_begin()) return 0;
  gl_fallback_list_clear(&gl_fallback_active);
  gl_quad_count = 0;
  gl_eviction_logged = 0;
  memset(gl_row_submitted, 0, gl_row_submitted_count);
  gl_clear_background();
  return 1;
}

static int gl_has_drawables(void) {
  size_t i;

  if (gl_quad_count) return 1;
  for (i = 0; i < gl_row_count; i++) {
    if (gl_row_batches[i].valid && gl_row_batches[i].count) return 1;
  }
  return 0;
}

static void gl_present(void) {
  if (gl_copy_sub_buffer) {
    gl_copy_sub_buffer(gl_dpy, gl_win, 0, 0, gl_window_width(),
                       gl_window_height());
  } else {
    glXSwapBuffers(gl_dpy, gl_win);
  }
}

static void gl_frame_end(void) {
  int has_quads = gl_has_drawables();

  gl_flush_glyphs();
  if (has_quads) gl_present();
  gl_fallback_replay(&gl_fallback_active);
}

static void gl_resize(int col, int row) {
  render_xft_ops.resize(col, row);
  gl_rows_invalidate_all();
  gl_fallback_list_clear(&gl_fallback_active);
  gl_quad_count = 0;
  gl_viewport();
  gl_clear_background();
  gl_present();
}

static int gl_is_background_color(const RenderColor* color) {
  RenderColor bg = gl_background_color();

  return color->red == bg.red && color->green == bg.green &&
         color->blue == bg.blue && color->alpha == bg.alpha;
}

static void gl_clear_rect(int x, int y, int width, int height,
                          const RenderColor* color) {
  if (gl_is_background_color(color)) return;
  gl_append_rect(x, y, width, height, color);
}

static int gl_line_has_image(Line line, int x1, int x2) {
  int x;

  for (x = x1; x < x2; x++) {
    if (line[x].mode & ATTR_IMAGE) return 1;
  }
  return 0;
}

static void gl_draw_line(Line line, int x1, int y1, int x2) {
  GlRowBatch* row;

  if (y1 < 0 || gl_frame_arrays_reserve((size_t)y1 + 1) != 0) {
    render_xft_ops.draw_line(line, x1, y1, x2);
    return;
  }
  row = &gl_row_batches[y1];
  if (gl_row_submitted[y1]) return;
  gl_row_submitted[y1] = 1;

  if (gl_line_has_image(line, x1, x2)) {
    render_xft_ops.draw_line(line, x1, y1, x2);
    return;
  }

  if (!gl_frame_dirty[y1] && row->valid) {
    gl_row_replay(row);
    return;
  }

  gl_row_clear(row);
  gl_quad_count = 0;
  gl_build_row = y1;
  render_xft_ops.draw_line(line, x1, y1, x2);
  gl_build_row = -1;
  gl_row_set(row, gl_quad_data, gl_quad_count);
  row->upload_pending = 1;
  gl_quad_count = 0;
}

static void gl_draw_cursor(int cx, int cy, Glyph g, int ox, int oy, Glyph og,
                           Line line, int len) {
  render_xft_ops.draw_cursor(cx, cy, g, ox, oy, og, line, len);
}

static void gl_boxdraw_fill(const RenderColor* color, int x, int y, int width,
                            int height) {
  gl_append_rect(x, y, width, height, color);
}

void render_gl_init(void) {
  boxdraw_set_fill_rect(gl_boxdraw_fill);
  gl_rows_invalidate_all();
}

static void gl_clipped_rect(const RenderColor* color, int x, int y, int width,
                            int height, int clipx, int clipy, int clipright,
                            int clipbottom) {
  int x0 = MAX(x, clipx);
  int y0 = MAX(y, clipy);
  int x1 = MIN(x + width, clipright);
  int y1 = MIN(y + height, clipbottom);

  gl_append_rect(x0, y0, x1 - x0, y1 - y0, color);
}

static void gl_draw_dashed(const RenderColor* color, int x, int y, int width,
                           int wavelen, float fraction, int thick, int clipx,
                           int clipright) {
  int dashw = MAX(1, (int)(fraction * wavelen));

  for (int i = x - x % wavelen; i < x + width; i += wavelen) {
    int startx = MAX(i, x);
    int endx = MIN(i + dashw, x + width);
    int visiblex = MAX(startx, clipx);
    int visiblew = MIN(endx, clipright) - visiblex;
    if (visiblew > 0) gl_append_rect(visiblex, y, visiblew, thick, color);
  }
}

static void gl_draw_curl_line(const RenderColor* color, int x0, int y0, int x1,
                              int y1, int thick, int clipx, int clipy,
                              int clipright, int clipbottom) {
  int dx = x1 - x0;
  int dy = y1 - y0;
  int steps = MAX(abs(dx), abs(dy));
  int half = thick / 2;

  if (steps <= 0) {
    gl_clipped_rect(color, x0 - half, y0 - half, thick + 1, thick + 1, clipx,
                    clipy, clipright, clipbottom);
    return;
  }
  for (int i = 0; i <= steps; i++) {
    int px = x0 + dx * i / steps;
    int py = y0 + dy * i / steps;
    gl_clipped_rect(color, px - half, py - half, thick + 1, thick + 1, clipx,
                    clipy, clipright, clipbottom);
  }
}

static void gl_draw_curl(const RenderColor* color, int x, int y, int width,
                         int height, int thick, int clipx, int clipy,
                         int clipright, int clipbottom) {
  int yoffset = thick / 2;
  int segh = MAX(1, height - thick);
  int segw = segh;
  int wavelen = MAX(1, segw * 2);

  for (int i = x - x % wavelen; i < x + width; i += wavelen) {
    gl_draw_curl_line(color, i, y + yoffset, i + segw, y + yoffset + segh,
                      thick, clipx, clipy, clipright, clipbottom);
    gl_draw_curl_line(color, i + segw, y + yoffset + segh, i + wavelen,
                      y + yoffset, thick, clipx, clipy, clipright, clipbottom);
  }
}

static void gl_draw_underline(const RenderColor* color, int x, int y, int width,
                              uint32_t style, int thick, float fthick,
                              int cell_width, int cell_top, int cell_bottom) {
  int clipright = x + width;

  if (style == UNDERLINE_DOUBLE) {
    int doubleh = thick * 2 + (int)ceilf(fthick * 0.5f);
    int liney = y - MAX(0, y + doubleh - cell_bottom);

    gl_clipped_rect(color, x, liney, width, thick, x, cell_top, clipright,
                    cell_bottom);
    gl_clipped_rect(color, x, liney + doubleh - thick, width, thick, x,
                    cell_top, clipright, cell_bottom);
  } else if (style == UNDERLINE_DOTTED) {
    gl_draw_dashed(color, x, y, width, thick * 2, 0.5f, thick, x, clipright);
  } else if (style == UNDERLINE_DASHED) {
    int wavelen = MAX(2, (int)(cell_width * 0.9f));
    gl_draw_dashed(color, x, y, width, wavelen, 0.65f, thick, x, clipright);
  } else if (style == UNDERLINE_CURLY) {
    int curlh = thick * 2 + (int)roundf(fthick * 0.75f);
    int liney = y - MAX(0, y + curlh - cell_bottom);

    gl_draw_curl(color, x, liney, width, curlh, thick, x, cell_top, clipright,
                 cell_bottom);
  } else {
    gl_clipped_rect(color, x, y, width, thick, x, cell_top, clipright,
                    cell_bottom);
  }
}

static void gl_draw_strike(const RenderColor* color, int x, int y, int width,
                           int thick) {
  gl_append_rect(x, y, width, thick, color);
}

static void gl_draw_images(Glyph base, Line line, int x1, int y1, int x2) {
  render_xft_ops.draw_images(base, line, x1, y1, x2);
}

static void gl_draw_image_cell(Glyph g, int x, int y) {
  render_xft_ops.draw_image_cell(g, x, y);
}

static void gl_images_begin(int* dirty, int rows) {
  size_t i;

  if (gl_frame_arrays_reserve((size_t)rows) == 0) {
    memcpy(gl_frame_dirty, dirty, (size_t)rows * sizeof(*dirty));
    if (gl_full_rebuild) {
      for (i = 0; i < (size_t)rows; i++) gl_frame_dirty[i] = 1;
      gl_full_rebuild = 0;
    }
    for (i = 0; i < (size_t)rows; i++) {
      if (gl_frame_dirty[i] || !gl_row_batches[i].valid) continue;
      gl_row_replay(&gl_row_batches[i]);
      gl_row_submitted[i] = 1;
    }
  }
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

static void gl_resources_reset(void) {
  gl_fallback_list_clear(&gl_fallback_active);
  gl_rows_invalidate_all();
  gl_quad_count = 0;
  gl_atlas_reset();
}

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
    .draw_glyphs = gl_draw_glyphs,
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
  gl_copy_sub_buffer = (gl_copy_sub_buffer_mesa_fn)glXGetProcAddress(
      (const GLubyte*)"glXCopySubBufferMESA");
  if (gl_atlas_create() != 0)
    fputs("st: glyph atlas allocation failed\n", stderr);
  if (gl_text_create() != 0)
    fputs("st: glyph shader setup failed\n", stderr);
  gl_setup_vsync(dpy, win, vis->screen);
  return 1;
}

void gl_deinit(void) {
  Display* dpy = gl_dpy;
  GLXContext ctx = gl_ctx;

  if (gl_fallback_draw && dpy) XftDrawDestroy(gl_fallback_draw);
  gl_fallback_draw = NULL;
  gl_fallback_list_clear(&gl_fallback_active);
  gl_text_destroy();
  gl_atlas_destroy();
  gl_dpy = NULL;
  gl_ctx = NULL;
  gl_win = None;
  gl_copy_sub_buffer = NULL;
  if (!dpy) return;
  glXMakeContextCurrent(dpy, None, None, NULL);
  if (ctx) glXDestroyContext(dpy, ctx);
}
