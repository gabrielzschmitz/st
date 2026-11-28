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

/** GLX renderer implementation. */
extern const RendererOps render_gl_ops;

/** Install GL callbacks used outside the RendererOps table. */
void render_gl_init(void);

/** Window geometry and effective background color for GL presentation. */
int gl_window_width(void);
int gl_window_height(void);
RenderColor gl_background_color(void);

/**
 * Create and make current a GLX context for the given window.
 * On failure, set reason to a short description of the missing GLX piece.
 */
int gl_init(Display* dpy, Window win, XVisualInfo* vis, const char** reason);

/** Release the current GLX context. */
void gl_deinit(void);

#endif
