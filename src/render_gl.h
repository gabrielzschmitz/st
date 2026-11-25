/**
 * @file render_gl.h
 * @brief Optional GLX renderer backend.
 */

/* See LICENSE for license details. */

#ifndef ST_RENDER_GL_H
#define ST_RENDER_GL_H

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include "render.h"

/** GLX renderer implementation. Primitives still draw to Xft offscreen. */
extern const RendererOps render_gl_ops;

/** Window geometry and effective background color for GL presentation. */
int gl_window_width(void);
int gl_window_height(void);
RenderColor gl_background_color(void);

/** Create and make current a GLX context for the given window. */
int gl_init(Display* dpy, Window win, XVisualInfo* vis);

/** Release the current GLX context. */
void gl_deinit(void);

#endif
