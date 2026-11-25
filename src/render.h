/**
 * @file render.h
 * @brief Renderer boundary for terminal drawing.
 */

/* See LICENSE for license details. */

#ifndef ST_RENDER_H
#define ST_RENDER_H

#include "st.h"

typedef struct RendererOps RendererOps;

/**
 * Backend drawing operations. Implementations own pixel-level work while
 * st.c and x.c keep terminal state and cell layout decisions.
 */
struct RendererOps {
  const char* name;

  /* Frame lifecycle. */
  int (*frame_begin)(void);
  void (*frame_end)(void);
  void (*resize)(int width, int height);

  /* Primitives. RenderColor mirrors XftColor's storage so backends can read
   * channels without exposing a particular drawing library. */
  void (*clear_rect)(int x, int y, int width, int height,
                     const RenderColor* color);
  void (*draw_line)(Line line, int x1, int y1, int x2);
  void (*draw_cursor)(int cx, int cy, Glyph g, int ox, int oy, Glyph og,
                      Line line, int len);
  void (*draw_underline)(const RenderColor* color, int x, int y, int width,
                         uint32_t style, int thick, float fthick,
                         int cell_width, int cell_bottom);
  void (*draw_strike)(const RenderColor* color, int x, int y, int width,
                      int thick);

  /* Kitty image drawing. */
  void (*draw_images)(Glyph base, Line line, int x1, int y1, int x2);
  void (*draw_image_cell)(Glyph g, int x, int y);
  void (*images_begin)(int* dirty, int rows);
  void (*images_end)(void);

  /* Image resource management, unused until the GL backend. */
  int (*image_upload)(uint32_t image_id, int width, int height,
                      const void* rgba);
  void (*image_free)(uint32_t image_id);
  void (*resources_reset)(void);
};

/** Xft renderer implementation. */
extern const RendererOps render_xft_ops;

/** Install Xft callbacks used outside the RendererOps table. */
void render_xft_init(void);

/** Return the active renderer. */
const RendererOps* renderer_current(void);

/** Select a renderer by name. An unknown name is fatal. */
void renderer_select(const char* name);

/** Release renderer-owned resources before exit. */
void renderer_deinit(void);

#endif
