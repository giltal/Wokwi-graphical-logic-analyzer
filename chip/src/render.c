#include "render.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "font.h"
#include "wokwi-api.h"

/* Heap allocated (614 KB): the simulator only provides 2 pages of initial
 * memory, so this must not live in the static data section. */
static uint32_t *g_backbuf;
static buffer_t g_fb;
static bool g_fb_ready;

/* Dirty row range, half-open [top, bot). */
static int g_dirty_top = SCREEN_H;
static int g_dirty_bot = 0;

static void mark_dirty(int y0, int y1) {
  if (y0 < 0) {
    y0 = 0;
  }
  if (y1 > SCREEN_H) {
    y1 = SCREEN_H;
  }
  if (y0 >= y1) {
    return;
  }
  if (y0 < g_dirty_top) {
    g_dirty_top = y0;
  }
  if (y1 > g_dirty_bot) {
    g_dirty_bot = y1;
  }
}

bool render_init(void) {
  uint32_t w = 0;
  uint32_t h = 0;
  g_fb = framebuffer_init(&w, &h);
  if (w != SCREEN_W || h != SCREEN_H) {
    printf("[logic-scope] display is %ux%u but firmware expects %dx%d\n", w, h,
           SCREEN_W, SCREEN_H);
    return false;
  }
  g_backbuf = malloc(sizeof(uint32_t) * SCREEN_W * SCREEN_H);
  if (g_backbuf == NULL) {
    printf("[logic-scope] out of memory: cannot allocate the %dx%d back buffer\n",
           SCREEN_W, SCREEN_H);
    return false;
  }
  g_fb_ready = true;
  render_mark_all_dirty();
  return true;
}

void render_mark_all_dirty(void) {
  g_dirty_top = 0;
  g_dirty_bot = SCREEN_H;
}

void render_flush(void) {
  if (!g_fb_ready || g_dirty_top >= g_dirty_bot) {
    return;
  }
  const uint32_t offset = (uint32_t)g_dirty_top * SCREEN_W * 4u;
  const uint32_t len = (uint32_t)(g_dirty_bot - g_dirty_top) * SCREEN_W * 4u;
  buffer_write(g_fb, offset, (uint8_t *)&g_backbuf[g_dirty_top * SCREEN_W], len);
  g_dirty_top = SCREEN_H;
  g_dirty_bot = 0;
}

void fb_clear(uint32_t color) {
  for (int i = 0; i < SCREEN_W * SCREEN_H; i++) {
    g_backbuf[i] = color;
  }
  render_mark_all_dirty();
}

void fb_pixel(int x, int y, uint32_t color) {
  if ((unsigned)x >= (unsigned)SCREEN_W || (unsigned)y >= (unsigned)SCREEN_H) {
    return;
  }
  g_backbuf[y * SCREEN_W + x] = color;
  mark_dirty(y, y + 1);
}

void fb_fill_rect(int x, int y, int w, int h, uint32_t color) {
  if (w <= 0 || h <= 0) {
    return;
  }
  int x0 = x < 0 ? 0 : x;
  int y0 = y < 0 ? 0 : y;
  int x1 = x + w;
  int y1 = y + h;
  if (x1 > SCREEN_W) {
    x1 = SCREEN_W;
  }
  if (y1 > SCREEN_H) {
    y1 = SCREEN_H;
  }
  if (x0 >= x1 || y0 >= y1) {
    return;
  }
  for (int yy = y0; yy < y1; yy++) {
    uint32_t *row = &g_backbuf[yy * SCREEN_W];
    for (int xx = x0; xx < x1; xx++) {
      row[xx] = color;
    }
  }
  mark_dirty(y0, y1);
}

void fb_hline(int x, int y, int w, uint32_t color) {
  fb_fill_rect(x, y, w, 1, color);
}

void fb_vline(int x, int y, int h, uint32_t color) {
  fb_fill_rect(x, y, 1, h, color);
}

void fb_rect(int x, int y, int w, int h, uint32_t color) {
  if (w <= 0 || h <= 0) {
    return;
  }
  fb_hline(x, y, w, color);
  fb_hline(x, y + h - 1, w, color);
  fb_vline(x, y, h, color);
  fb_vline(x + w - 1, y, h, color);
}

void fb_vline_dotted(int x, int y, int h, uint32_t color, int period) {
  if (period < 1) {
    period = 1;
  }
  for (int i = 0; i < h; i += period) {
    fb_pixel(x, y + i, color);
  }
}

void fb_hline_dotted(int x, int y, int w, uint32_t color, int period) {
  if (period < 1) {
    period = 1;
  }
  for (int i = 0; i < w; i += period) {
    fb_pixel(x + i, y, color);
  }
}

static int glyph_index(unsigned char c) {
  if (c == 0x01) {
    return FONT_GLYPH_ARROW_UP;
  }
  if (c == 0x02) {
    return FONT_GLYPH_ARROW_DOWN;
  }
  if (c < 0x20 || c > 0x7E) {
    return 0; /* space */
  }
  return c - 0x20;
}

int fb_text_width_n(const char *s, int n, int scale) {
  (void)s;
  if (scale < 1) {
    scale = 1;
  }
  if (n <= 0) {
    return 0;
  }
  return (n * FONT_ADVANCE - 1) * scale;
}

int fb_text_width(const char *s, int scale) {
  int n = 0;
  while (s[n] != '\0') {
    n++;
  }
  return fb_text_width_n(s, n, scale);
}

int fb_text(int x, int y, const char *s, uint32_t color, int scale) {
  if (scale < 1) {
    scale = 1;
  }
  for (const unsigned char *p = (const unsigned char *)s; *p != '\0'; p++) {
    const uint8_t *glyph = kFont5x7[glyph_index(*p)];
    for (int col = 0; col < FONT_W; col++) {
      const uint8_t bits = glyph[col];
      if (bits == 0) {
        continue;
      }
      for (int row = 0; row < FONT_H; row++) {
        if (bits & (1u << row)) {
          if (scale == 1) {
            fb_pixel(x + col, y + row, color);
          } else {
            fb_fill_rect(x + col * scale, y + row * scale, scale, scale, color);
          }
        }
      }
    }
    x += FONT_ADVANCE * scale;
  }
  return x - scale;
}
