/*
 * render.h - framebuffer back buffer + drawing primitives for chip-logic-scope.
 *
 * All drawing goes into a static RGBA8888 back buffer. render_flush() pushes
 * only the rows that were touched since the last flush.
 */
#ifndef RENDER_H
#define RENDER_H

#include <stdbool.h>
#include <stdint.h>

#define SCREEN_W 480
#define SCREEN_H 320

/* Framebuffer byte order is R,G,B,A (verified in phase 0). */
#define RGBA(r, g, b) \
  (0xFF000000u | ((uint32_t)(b) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(r))

/* Must be called from chip_init(). Returns false if the display size does not
 * match SCREEN_W x SCREEN_H. */
bool render_init(void);

/* Push every dirty row to the simulator and reset the dirty range. */
void render_flush(void);

/* Force the next flush to push the whole screen. */
void render_mark_all_dirty(void);

void fb_clear(uint32_t color);
void fb_pixel(int x, int y, uint32_t color);
void fb_fill_rect(int x, int y, int w, int h, uint32_t color);
void fb_rect(int x, int y, int w, int h, uint32_t color);
void fb_hline(int x, int y, int w, uint32_t color);
void fb_vline(int x, int y, int h, uint32_t color);
/* Vertical line with `on` pixels drawn every `period` pixels. */
void fb_vline_dotted(int x, int y, int h, uint32_t color, int period);
void fb_hline_dotted(int x, int y, int w, uint32_t color, int period);

/* Draws `s` with a transparent background. Returns the x just past the text.
 * `scale` must be >= 1. */
int fb_text(int x, int y, const char *s, uint32_t color, int scale);

/* Pixel width of `s` when drawn at `scale`. */
int fb_text_width(const char *s, int scale);
int fb_text_width_n(const char *s, int n, int scale);

#endif /* RENDER_H */
