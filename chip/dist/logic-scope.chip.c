/*
 * chip-logic-scope - 8-channel graphical logic analyzer for Wokwi.
 *
 * GENERATED FILE - do not edit. Built from the sources in chip/src by
 * tools/amalgamate.js, so that the chip can be pasted into a wokwi.com
 * project, which compiles exactly one C file per custom chip.
 */

/* ==== capture.c =================================================== */
/* ==== capture.h =================================================== */
/*
 * capture.h - transition-based capture engine for chip-logic-scope.
 *
 * One entry is stored per change of the 8-bit input vector, never per sample,
 * so an idle bus costs nothing. The ring overwrites the oldest entry when full
 * (roll mode); `base_mask` always holds the level that was in effect *before*
 * the oldest retained event.
 */
#ifndef CAPTURE_H
#define CAPTURE_H

#include <stdbool.h>
#include <stdint.h>

#define SCOPE_CHANNELS 8

/* Must be a power of two. 32768 events = 512 KB. */
#define SCOPE_MAX_EVENTS 32768

typedef struct {
  uint64_t ts_ns;
  uint8_t mask;
} scope_event_t;

typedef struct {
  scope_event_t *events; /* heap allocated: static data must stay tiny, see below */
  uint32_t head;  /* next write slot */
  uint32_t count; /* retained events, <= SCOPE_MAX_EVENTS */
  uint32_t total; /* events ever recorded (wraps, display only) */
  uint8_t mask;       /* current level of all channels */
  uint8_t base_mask;  /* level before events[oldest] */
} capture_t;

/*
 * The simulator hands the chip an imported memory of only 2 pages (128 KB) and
 * lets it grow at runtime, so the ring must be malloc'd rather than static.
 * Returns false if the allocation failed.
 */
bool capture_init(capture_t *c, uint8_t initial_mask);

/* Appends an event. Must stay cheap: this runs on every edge of every channel. */
void capture_push(capture_t *c, uint64_t ts_ns, uint8_t mask);

/*
 * Summarises the window [t_start, t_start + width * ns_per_px) into per-column
 * level masks. For column x, bit n of col_high is set if channel n was high at
 * any point inside the column, and bit n of col_low if it was low. Both bits
 * set means the column contains at least one transition; bit n of col_multi is
 * set when it contains two or more (i.e. the column is aliased/a glitch).
 */
void capture_columns(const capture_t *c, uint64_t t_start, uint64_t ns_per_px,
                     int width, uint8_t *col_high, uint8_t *col_low,
                     uint8_t *col_multi);

/* ------------------------------------------------------------- queries -- */

/* Level of every channel at instant `t`. */
uint8_t capture_mask_at(const capture_t *c, uint64_t t);

/*
 * First transition of the channels in `bit_mask` inside [from, to). Writes the
 * timestamp to *ts and the mask after the transition to *mask_after.
 * Returns false when the window holds no such transition.
 */
bool capture_next_edge(const capture_t *c, uint64_t from, uint64_t to,
                       uint8_t bit_mask, uint64_t *ts, uint8_t *mask_after);

/* Same, but only transitions that leave `bit` at `level`. */
bool capture_next_level(const capture_t *c, uint64_t from, uint64_t to,
                        uint8_t bit, bool level, uint64_t *ts);

/* --------------------------------------------------------- measurements -- */

typedef struct {
  uint32_t edges;       /* transitions inside the window */
  uint64_t period_ns;   /* mean period, 0 when fewer than 2 rising edges */
  uint32_t duty_tenths; /* duty cycle in 0.1 % units */
  uint64_t min_pulse_ns; /* shortest complete high or low pulse, 0 if none */
} measure_t;

/* Measures one channel over [t0, t1). */
void capture_measure(const capture_t *c, uint64_t t0, uint64_t t1,
                     uint8_t channel, measure_t *m);

/* --------------------------------------------------------------- trigger -- */

typedef enum {
  TRIGGER_OFF = 0, /* roll mode: the view always tracks the newest data */
  TRIGGER_EDGE,    /* normal sweep: wait for the edge, then hold the frame */
  TRIGGER_LEVEL,   /* fire as soon as the channel sits at the wanted level */
  TRIGGER_AUTO,    /* like EDGE, but rolls while no edge arrives */
  TRIGGER_SINGLE,  /* one sweep, then hold until re-armed */
  TRIGGER_MODE_COUNT
} trigger_mode_t;

/* In LEVEL mode this selects the level: RISING/BOTH = high, FALLING = low. */
typedef enum {
  TRIGGER_RISING = 0,
  TRIGGER_FALLING,
  TRIGGER_BOTH,
  TRIGGER_EDGE_COUNT
} trigger_edge_t;

typedef enum {
  TRIGGER_IDLE = 0, /* stopped, or mode == TRIGGER_OFF */
  TRIGGER_ARMED,    /* waiting for the trigger condition */
  TRIGGER_FILLING,  /* fired, collecting the post-trigger window */
  TRIGGER_HOLD      /* sweep complete, frame frozen */
} trigger_state_t;

typedef struct {
  uint8_t mode;        /* trigger_mode_t */
  uint8_t channel;     /* 0..SCOPE_CHANNELS-1 */
  uint8_t edge;        /* trigger_edge_t */
  uint8_t pre_percent; /* share of the window shown before the trigger */
  uint8_t state;       /* trigger_state_t */
  uint64_t armed_ns;
  uint64_t fire_ns;
} trigger_t;

void trigger_idle(trigger_t *t);

/* Arms the trigger; LEVEL mode fires immediately if `mask` already matches. */
void trigger_arm(trigger_t *t, uint64_t now_ns, uint8_t mask);

/* Hot path: called from the pin change callback whenever the mask changed. */
void trigger_on_change(trigger_t *t, uint64_t ts_ns, uint8_t prev, uint8_t next);

#endif /* CAPTURE_H */



#include <stdlib.h>

#define RING_MASK (SCOPE_MAX_EVENTS - 1)

/* Physical slot of logical index `i` (0 = oldest retained event). */
static inline uint32_t phys_index(const capture_t *c, uint32_t i) {
  return ((c->head - c->count) + i) & RING_MASK;
}

bool capture_init(capture_t *c, uint8_t initial_mask) {
  c->events = malloc(sizeof(scope_event_t) * SCOPE_MAX_EVENTS);
  c->head = 0;
  c->count = 0;
  c->total = 0;
  c->mask = initial_mask;
  c->base_mask = initial_mask;
  return c->events != NULL;
}

void capture_push(capture_t *c, uint64_t ts_ns, uint8_t mask) {
  if (c->count == SCOPE_MAX_EVENTS) {
    /* head points at the oldest event, which is about to be overwritten. */
    c->base_mask = c->events[c->head].mask;
  } else {
    c->count++;
  }
  c->events[c->head].ts_ns = ts_ns;
  c->events[c->head].mask = mask;
  c->head = (c->head + 1) & RING_MASK;
  c->mask = mask;
  c->total++;
}

/* First logical index whose timestamp is >= ts, or count if none. */
static uint32_t lower_bound(const capture_t *c, uint64_t ts) {
  uint32_t lo = 0;
  uint32_t hi = c->count;
  while (lo < hi) {
    const uint32_t mid = lo + (hi - lo) / 2;
    if (c->events[phys_index(c, mid)].ts_ns < ts) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return lo;
}

void capture_columns(const capture_t *c, uint64_t t_start, uint64_t ns_per_px,
                     int width, uint8_t *col_high, uint8_t *col_low,
                     uint8_t *col_multi) {
  if (ns_per_px == 0) {
    ns_per_px = 1;
  }

  uint32_t idx = lower_bound(c, t_start);
  uint8_t cur = (idx == 0) ? c->base_mask : c->events[phys_index(c, idx - 1)].mask;

  for (int x = 0; x < width; x++) {
    const uint64_t col_end = t_start + (uint64_t)(x + 1) * ns_per_px;
    uint8_t high = cur;
    uint8_t low = (uint8_t)~cur;
    uint8_t seen = 0;  /* channels that changed once in this column */
    uint8_t multi = 0; /* ... and changed again */

    while (idx < c->count) {
      const scope_event_t *e = &c->events[phys_index(c, idx)];
      if (e->ts_ns >= col_end) {
        break;
      }
      const uint8_t changed = (uint8_t)(cur ^ e->mask);
      multi |= (uint8_t)(changed & seen);
      seen |= changed;
      cur = e->mask;
      high |= cur;
      low |= (uint8_t)~cur;
      idx++;
    }

    col_high[x] = high;
    col_low[x] = low;
    col_multi[x] = multi;
  }
}

/* ------------------------------------------------------------- queries -- */

uint8_t capture_mask_at(const capture_t *c, uint64_t t) {
  /* lower_bound gives the first event at or after t, so the level in effect at
   * t is the mask of the event before it. */
  const uint32_t idx = lower_bound(c, t + 1ull);
  return (idx == 0) ? c->base_mask : c->events[phys_index(c, idx - 1)].mask;
}

bool capture_next_edge(const capture_t *c, uint64_t from, uint64_t to,
                       uint8_t bit_mask, uint64_t *ts, uint8_t *mask_after) {
  uint32_t idx = lower_bound(c, from);
  uint8_t cur = (idx == 0) ? c->base_mask : c->events[phys_index(c, idx - 1)].mask;

  for (; idx < c->count; idx++) {
    const scope_event_t *e = &c->events[phys_index(c, idx)];
    if (e->ts_ns >= to) {
      break;
    }
    if ((cur ^ e->mask) & bit_mask) {
      *ts = e->ts_ns;
      *mask_after = e->mask;
      return true;
    }
    cur = e->mask;
  }
  return false;
}

bool capture_next_level(const capture_t *c, uint64_t from, uint64_t to,
                        uint8_t bit, bool level, uint64_t *ts) {
  uint64_t t = from;
  uint8_t after;
  while (capture_next_edge(c, t, to, bit, &t, &after)) {
    if (((after & bit) != 0u) == level) {
      *ts = t;
      return true;
    }
    t++; /* skip past this transition */
  }
  return false;
}

/* --------------------------------------------------------- measurements -- */
void capture_measure(const capture_t *c, uint64_t t0, uint64_t t1,
                     uint8_t channel, measure_t *m) {
  m->edges = 0;
  m->period_ns = 0;
  m->duty_tenths = 0;
  m->min_pulse_ns = 0;

  if (t1 <= t0) {
    return;
  }
  const uint8_t bit = (uint8_t)(1u << channel);

  uint32_t idx = lower_bound(c, t0);
  uint8_t cur = (idx == 0) ? c->base_mask : c->events[phys_index(c, idx - 1)].mask;

  uint64_t high_ns = 0;
  uint64_t seg_start = t0;    /* start of the current level segment */
  uint64_t first_rise = 0;
  uint64_t last_rise = 0;
  uint32_t rises = 0;

  for (; idx < c->count; idx++) {
    const scope_event_t *e = &c->events[phys_index(c, idx)];
    if (e->ts_ns >= t1) {
      break;
    }
    if (((cur ^ e->mask) & bit) == 0u) {
      cur = e->mask;
      continue;
    }

    const uint64_t width = e->ts_ns - seg_start;
    if (cur & bit) {
      high_ns += width;
    }
    /* Only segments bounded by two transitions are complete pulses. */
    if (m->edges > 0 && (m->min_pulse_ns == 0 || width < m->min_pulse_ns)) {
      m->min_pulse_ns = width;
    }

    cur = e->mask;
    seg_start = e->ts_ns;
    m->edges++;
    if (cur & bit) {
      if (rises == 0) {
        first_rise = e->ts_ns;
      }
      last_rise = e->ts_ns;
      rises++;
    }
  }

  if (cur & bit) {
    high_ns += t1 - seg_start;
  }

  m->duty_tenths = (uint32_t)((high_ns * 1000ull) / (t1 - t0));
  if (rises >= 2) {
    m->period_ns = (last_rise - first_rise) / (rises - 1u);
  }
}

/* --------------------------------------------------------------- trigger -- */

void trigger_idle(trigger_t *t) {
  t->state = TRIGGER_IDLE;
  t->fire_ns = 0;
}

void trigger_arm(trigger_t *t, uint64_t now_ns, uint8_t mask) {
  t->state = TRIGGER_ARMED;
  t->armed_ns = now_ns;
  t->fire_ns = 0;

  if (t->mode == TRIGGER_LEVEL) {
    const bool high = (mask & (uint8_t)(1u << t->channel)) != 0;
    const bool want_high = (t->edge != TRIGGER_FALLING);
    if (high == want_high) {
      t->state = TRIGGER_FILLING;
      t->fire_ns = now_ns;
    }
  }
}

void trigger_on_change(trigger_t *t, uint64_t ts_ns, uint8_t prev,
                       uint8_t next) {
  if (t->state != TRIGGER_ARMED) {
    return;
  }
  const uint8_t bit = (uint8_t)(1u << t->channel);
  if (((prev ^ next) & bit) == 0u) {
    return;
  }
  const bool rising = (next & bit) != 0;
  if (t->edge == TRIGGER_RISING && !rising) {
    return;
  }
  if (t->edge == TRIGGER_FALLING && rising) {
    return;
  }
  t->state = TRIGGER_FILLING;
  t->fire_ns = ts_ns;
}


/* ==== render.c ==================================================== */
/* ==== render.h ==================================================== */
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



#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ==== font.h ====================================================== */
/*
 * font.h - 5x7 bitmap font, column-major, LSB = top row.
 *
 * Index 0   .. 94  -> ASCII 0x20 ('space') .. 0x7E ('~')
 * Index 95        -> arrow up    (write '\x01' in a string)
 * Index 96        -> arrow down  (write '\x02' in a string)
 *
 * Each glyph is 5 columns wide; the renderer adds 1 column of spacing,
 * so the advance is 6 pixels and the cell is 6x8.
 */
#ifndef SCOPE_FONT_H_
#define SCOPE_FONT_H_

#include <stdint.h>

#define FONT_W 5
#define FONT_H 7
#define FONT_ADVANCE 6
#define FONT_CELL_H 8

#define FONT_GLYPH_ARROW_UP 95
#define FONT_GLYPH_ARROW_DOWN 96
#define FONT_GLYPH_COUNT 97

/* Escape codes usable directly inside string literals. */
#define GLYPH_UP "\x01"
#define GLYPH_DOWN "\x02"

static const uint8_t kFont5x7[FONT_GLYPH_COUNT][FONT_W] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, /*   */
    {0x00, 0x00, 0x5F, 0x00, 0x00}, /* ! */
    {0x00, 0x07, 0x00, 0x07, 0x00}, /* " */
    {0x14, 0x7F, 0x14, 0x7F, 0x14}, /* # */
    {0x24, 0x2A, 0x7F, 0x2A, 0x12}, /* $ */
    {0x23, 0x13, 0x08, 0x64, 0x62}, /* % */
    {0x36, 0x49, 0x55, 0x22, 0x50}, /* & */
    {0x00, 0x05, 0x03, 0x00, 0x00}, /* ' */
    {0x00, 0x1C, 0x22, 0x41, 0x00}, /* ( */
    {0x00, 0x41, 0x22, 0x1C, 0x00}, /* ) */
    {0x14, 0x08, 0x3E, 0x08, 0x14}, /* * */
    {0x08, 0x08, 0x3E, 0x08, 0x08}, /* + */
    {0x00, 0x50, 0x30, 0x00, 0x00}, /* , */
    {0x08, 0x08, 0x08, 0x08, 0x08}, /* - */
    {0x00, 0x60, 0x60, 0x00, 0x00}, /* . */
    {0x20, 0x10, 0x08, 0x04, 0x02}, /* / */
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, /* 0 */
    {0x00, 0x42, 0x7F, 0x40, 0x00}, /* 1 */
    {0x42, 0x61, 0x51, 0x49, 0x46}, /* 2 */
    {0x21, 0x41, 0x45, 0x4B, 0x31}, /* 3 */
    {0x18, 0x14, 0x12, 0x7F, 0x10}, /* 4 */
    {0x27, 0x45, 0x45, 0x45, 0x39}, /* 5 */
    {0x3C, 0x4A, 0x49, 0x49, 0x30}, /* 6 */
    {0x01, 0x71, 0x09, 0x05, 0x03}, /* 7 */
    {0x36, 0x49, 0x49, 0x49, 0x36}, /* 8 */
    {0x06, 0x49, 0x49, 0x29, 0x1E}, /* 9 */
    {0x00, 0x36, 0x36, 0x00, 0x00}, /* : */
    {0x00, 0x56, 0x36, 0x00, 0x00}, /* ; */
    {0x00, 0x08, 0x14, 0x22, 0x41}, /* < */
    {0x14, 0x14, 0x14, 0x14, 0x14}, /* = */
    {0x41, 0x22, 0x14, 0x08, 0x00}, /* > */
    {0x02, 0x01, 0x51, 0x09, 0x06}, /* ? */
    {0x32, 0x49, 0x79, 0x41, 0x3E}, /* @ */
    {0x7E, 0x11, 0x11, 0x11, 0x7E}, /* A */
    {0x7F, 0x49, 0x49, 0x49, 0x36}, /* B */
    {0x3E, 0x41, 0x41, 0x41, 0x22}, /* C */
    {0x7F, 0x41, 0x41, 0x22, 0x1C}, /* D */
    {0x7F, 0x49, 0x49, 0x49, 0x41}, /* E */
    {0x7F, 0x09, 0x09, 0x01, 0x01}, /* F */
    {0x3E, 0x41, 0x41, 0x51, 0x32}, /* G */
    {0x7F, 0x08, 0x08, 0x08, 0x7F}, /* H */
    {0x00, 0x41, 0x7F, 0x41, 0x00}, /* I */
    {0x20, 0x40, 0x41, 0x3F, 0x01}, /* J */
    {0x7F, 0x08, 0x14, 0x22, 0x41}, /* K */
    {0x7F, 0x40, 0x40, 0x40, 0x40}, /* L */
    {0x7F, 0x02, 0x04, 0x02, 0x7F}, /* M */
    {0x7F, 0x04, 0x08, 0x10, 0x7F}, /* N */
    {0x3E, 0x41, 0x41, 0x41, 0x3E}, /* O */
    {0x7F, 0x09, 0x09, 0x09, 0x06}, /* P */
    {0x3E, 0x41, 0x51, 0x21, 0x5E}, /* Q */
    {0x7F, 0x09, 0x19, 0x29, 0x46}, /* R */
    {0x46, 0x49, 0x49, 0x49, 0x31}, /* S */
    {0x01, 0x01, 0x7F, 0x01, 0x01}, /* T */
    {0x3F, 0x40, 0x40, 0x40, 0x3F}, /* U */
    {0x1F, 0x20, 0x40, 0x20, 0x1F}, /* V */
    {0x7F, 0x20, 0x18, 0x20, 0x7F}, /* W */
    {0x63, 0x14, 0x08, 0x14, 0x63}, /* X */
    {0x03, 0x04, 0x78, 0x04, 0x03}, /* Y */
    {0x61, 0x51, 0x49, 0x45, 0x43}, /* Z */
    {0x00, 0x00, 0x7F, 0x41, 0x41}, /* [ */
    {0x02, 0x04, 0x08, 0x10, 0x20}, /* \ */
    {0x41, 0x41, 0x7F, 0x00, 0x00}, /* ] */
    {0x04, 0x02, 0x01, 0x02, 0x04}, /* ^ */
    {0x40, 0x40, 0x40, 0x40, 0x40}, /* _ */
    {0x00, 0x01, 0x02, 0x04, 0x00}, /* ` */
    {0x20, 0x54, 0x54, 0x54, 0x78}, /* a */
    {0x7F, 0x48, 0x44, 0x44, 0x38}, /* b */
    {0x38, 0x44, 0x44, 0x44, 0x20}, /* c */
    {0x38, 0x44, 0x44, 0x48, 0x7F}, /* d */
    {0x38, 0x54, 0x54, 0x54, 0x18}, /* e */
    {0x08, 0x7E, 0x09, 0x01, 0x02}, /* f */
    {0x08, 0x14, 0x54, 0x54, 0x3C}, /* g */
    {0x7F, 0x08, 0x04, 0x04, 0x78}, /* h */
    {0x00, 0x44, 0x7D, 0x40, 0x00}, /* i */
    {0x20, 0x40, 0x44, 0x3D, 0x00}, /* j */
    {0x7F, 0x10, 0x28, 0x44, 0x00}, /* k */
    {0x00, 0x41, 0x7F, 0x40, 0x00}, /* l */
    {0x7C, 0x04, 0x78, 0x04, 0x78}, /* m */
    {0x7C, 0x08, 0x04, 0x04, 0x78}, /* n */
    {0x38, 0x44, 0x44, 0x44, 0x38}, /* o */
    {0x7C, 0x14, 0x14, 0x14, 0x08}, /* p */
    {0x08, 0x14, 0x14, 0x18, 0x7C}, /* q */
    {0x7C, 0x08, 0x04, 0x04, 0x08}, /* r */
    {0x48, 0x54, 0x54, 0x54, 0x20}, /* s */
    {0x04, 0x3F, 0x44, 0x40, 0x20}, /* t */
    {0x3C, 0x40, 0x40, 0x20, 0x7C}, /* u */
    {0x1C, 0x20, 0x40, 0x20, 0x1C}, /* v */
    {0x3C, 0x40, 0x30, 0x40, 0x3C}, /* w */
    {0x44, 0x28, 0x10, 0x28, 0x44}, /* x */
    {0x0C, 0x50, 0x50, 0x50, 0x3C}, /* y */
    {0x44, 0x64, 0x54, 0x4C, 0x44}, /* z */
    {0x00, 0x08, 0x36, 0x41, 0x00}, /* { */
    {0x00, 0x00, 0x7F, 0x00, 0x00}, /* | */
    {0x00, 0x41, 0x36, 0x08, 0x00}, /* } */
    {0x08, 0x08, 0x2A, 0x1C, 0x08}, /* ~ */
    {0x04, 0x06, 0x7F, 0x06, 0x04}, /* arrow up   (\x01) */
    {0x10, 0x30, 0x7F, 0x30, 0x10}, /* arrow down (\x02) */
};

#endif /* SCOPE_FONT_H_ */


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


/* ==== decoders/decoder.c ========================================== */
/* ==== decoders/decoder.h ========================================== */
/*
 * decoder.h - protocol decoder interface for chip-logic-scope.
 *
 * Decoders replay the capture ring over the *visible window only*, so they need
 * no capture path of their own and no state between frames. Each one fills an
 * annotation list that the renderer draws as boxes in the decode lane.
 */
#ifndef DECODER_H
#define DECODER_H

#include <stdbool.h>
#include <stdint.h>


#define ANNOT_TEXT_MAX 12
#define ANNOT_MAX 192

typedef enum {
  ANNOT_FRAME = 0, /* protocol framing: START, STOP, address, ... */
  ANNOT_DATA,      /* payload */
  ANNOT_ERROR,     /* framing/parity/protocol error */
  ANNOT_KIND_COUNT
} annot_kind_t;

typedef struct {
  uint64_t t0;
  uint64_t t1;
  uint8_t kind; /* annot_kind_t */
  char text[ANNOT_TEXT_MAX];
} annot_t;

typedef struct {
  annot_t items[ANNOT_MAX];
  int count;
} annot_list_t;

void annot_reset(annot_list_t *l);

/* Appends one annotation; silently drops it when the list is full. */
void annot_add(annot_list_t *l, uint64_t t0, uint64_t t1, uint8_t kind,
               const char *text);

/* Formats `value` as hex (plus the ASCII character when printable). */
void annot_format_byte(char *dst, uint32_t value, int digits);

typedef enum {
  DECODER_NONE = 0,
  DECODER_UART,
  DECODER_I2C,
  DECODER_SPI,
  DECODER_COUNT
} decoder_id_t;

typedef struct {
  uint8_t ch[4];   /* channel per decoder line, see the per-decoder comment */
  uint32_t baud;   /* UART bit rate */
  uint8_t bits;    /* UART data bits, 5..9 */
  uint8_t parity;  /* 0 none, 1 even, 2 odd */
  uint8_t stop;    /* UART stop bits, 1 or 2 */
  uint8_t spi_mode; /* 0..3: CPOL = mode >> 1, CPHA = mode & 1 */
  uint8_t spi_msb;  /* 1 = MSB first, 0 = LSB first */
} decoder_cfg_t;

/* Short name shown in the decode lane gutter, "" for DECODER_NONE. */
const char *decoder_name(uint8_t id);

/* Runs the selected decoder over [t0, t1) and fills `out`. */
void decoder_run(uint8_t id, const capture_t *c, uint64_t t0, uint64_t t1,
                 const decoder_cfg_t *cfg, annot_list_t *out);

/* ch[0] = RX line. Async, so the decoder locks onto every start bit. */
void decode_uart(const capture_t *c, uint64_t t0, uint64_t t1,
                 const decoder_cfg_t *cfg, annot_list_t *out);

/* ch[0] = SDA, ch[1] = SCL. Clock-driven, so no bit rate is needed. */
void decode_i2c(const capture_t *c, uint64_t t0, uint64_t t1,
                const decoder_cfg_t *cfg, annot_list_t *out);

/* ch[0] = SCK, ch[1] = MOSI, ch[2] = CS (>= SCOPE_CHANNELS disables CS). */
void decode_spi(const capture_t *c, uint64_t t0, uint64_t t1,
                const decoder_cfg_t *cfg, annot_list_t *out);

#endif /* DECODER_H */



static const char *const kDecoderNames[DECODER_COUNT] = {
    "", "UART", "I2C", "SPI",
};

void annot_reset(annot_list_t *l) { l->count = 0; }

void annot_add(annot_list_t *l, uint64_t t0, uint64_t t1, uint8_t kind,
               const char *text) {
  if (l->count >= ANNOT_MAX) {
    return;
  }
  annot_t *a = &l->items[l->count++];
  a->t0 = t0;
  a->t1 = t1;
  a->kind = kind;
  int i = 0;
  while (text[i] != '\0' && i < ANNOT_TEXT_MAX - 1) {
    a->text[i] = text[i];
    i++;
  }
  a->text[i] = '\0';
}

void annot_format_byte(char *dst, uint32_t value, int digits) {
  static const char kHex[] = "0123456789ABCDEF";
  for (int i = digits - 1; i >= 0; i--) {
    *dst++ = kHex[(value >> (4 * i)) & 0x0Fu];
  }
  if (value >= 0x20u && value <= 0x7Eu) {
    *dst++ = ' ';
    *dst++ = '\'';
    *dst++ = (char)value;
    *dst++ = '\'';
  }
  *dst = '\0';
}

const char *decoder_name(uint8_t id) {
  return (id < DECODER_COUNT) ? kDecoderNames[id] : "";
}

void decoder_run(uint8_t id, const capture_t *c, uint64_t t0, uint64_t t1,
                 const decoder_cfg_t *cfg, annot_list_t *out) {
  annot_reset(out);
  switch (id) {
    case DECODER_UART:
      decode_uart(c, t0, t1, cfg, out);
      break;
    case DECODER_I2C:
      decode_i2c(c, t0, t1, cfg, out);
      break;
    case DECODER_SPI:
      decode_spi(c, t0, t1, cfg, out);
      break;
    default:
      break;
  }
}


/* ==== decoders/uart.c ============================================= */
/*
 * uart.c - asynchronous serial decoder.
 *
 * Idle line is high. Every falling edge is a candidate start bit; data bits are
 * sampled in the middle of each bit cell (LSB first), then the optional parity
 * bit and the stop bit(s) are checked. A frame that would run past the end of
 * the visible window is left undecoded until it is fully captured.
 */

#define UART_MIN_BITS 5
#define UART_MAX_BITS 9

void decode_uart(const capture_t *c, uint64_t t0, uint64_t t1,
                 const decoder_cfg_t *cfg, annot_list_t *out) {
  if (cfg->baud == 0u || t1 <= t0) {
    return;
  }
  const uint64_t bit_ns = 1000000000ull / cfg->baud;
  if (bit_ns == 0ull) {
    return; /* faster than 1 GBd: below the ns resolution of the capture */
  }

  uint32_t data_bits = cfg->bits;
  if (data_bits < UART_MIN_BITS) {
    data_bits = UART_MIN_BITS;
  } else if (data_bits > UART_MAX_BITS) {
    data_bits = UART_MAX_BITS;
  }
  const uint32_t parity_bits = (cfg->parity != 0u) ? 1u : 0u;
  const uint32_t stop_bits = (cfg->stop >= 2u) ? 2u : 1u;
  const uint8_t bit = (uint8_t)(1u << (cfg->ch[0] & 7u));

  uint64_t t = t0;
  while (out->count < ANNOT_MAX) {
    uint64_t start;
    if (!capture_next_level(c, t, t1, bit, false, &start)) {
      break;
    }

    const uint64_t frame_end =
        start + (uint64_t)(1u + data_bits + parity_bits + stop_bits) * bit_ns;
    if (frame_end > t1) {
      break; /* wait until the whole frame is inside the window */
    }

    /* Middle of bit cell n, counting the start bit as cell 0. */
    const uint64_t mid0 = start + bit_ns + bit_ns / 2ull;

    uint32_t value = 0;
    uint32_t ones = 0;
    for (uint32_t i = 0; i < data_bits; i++) {
      if (capture_mask_at(c, mid0 + (uint64_t)i * bit_ns) & bit) {
        value |= 1u << i; /* LSB first */
        ones++;
      }
    }

    bool parity_ok = true;
    if (parity_bits != 0u) {
      const bool p =
          (capture_mask_at(c, mid0 + (uint64_t)data_bits * bit_ns) & bit) != 0u;
      /* Even parity makes the total number of ones even, odd makes it odd. */
      const bool expected = (cfg->parity == 1u) ? ((ones & 1u) != 0u)
                                                : ((ones & 1u) == 0u);
      parity_ok = (p == expected);
    }

    const uint64_t stop_mid =
        mid0 + (uint64_t)(data_bits + parity_bits) * bit_ns;
    const bool stop_ok = (capture_mask_at(c, stop_mid) & bit) != 0u;

    char text[ANNOT_TEXT_MAX];
    uint8_t kind = ANNOT_DATA;
    if (!stop_ok) {
      text[0] = 'F';
      text[1] = 'R';
      text[2] = 'M';
      text[3] = '\0';
      kind = ANNOT_ERROR;
    } else if (!parity_ok) {
      text[0] = 'P';
      text[1] = 'A';
      text[2] = 'R';
      text[3] = '\0';
      kind = ANNOT_ERROR;
    } else {
      annot_format_byte(text, value, (data_bits > 8u) ? 3 : 2);
    }
    annot_add(out, start, frame_end, kind, text);

    /* Resume just before the stop bit ends so back-to-back frames are found. */
    t = frame_end - bit_ns / 2ull;
  }
}


/* ==== decoders/i2c.c ============================================== */
/*
 * i2c.c - two-wire (I2C) decoder.
 *
 * The bus is self-clocked, so there is nothing to configure beyond the two
 * channels: SDA on ch[0], SCL on ch[1].
 *
 *   START   SDA falls while SCL is high
 *   STOP    SDA rises while SCL is high
 *   bit     sampled on every SCL rising edge, MSB first
 *   byte    8 bits + a 9th clock carrying ACK (low) or NAK (high)
 *
 * The first byte after a START (or a repeated START) is the address, drawn as
 * "68 W" / "68 R"; the rest are data bytes. A NAK turns the box red.
 */

/* "68 W" - 7-bit address plus the R/W flag of the first byte after a START. */
static void format_addr(char *dst, uint32_t value) {
  static const char kHex[] = "0123456789ABCDEF";
  const uint32_t addr = value >> 1;
  *dst++ = kHex[(addr >> 4) & 0x0Fu];
  *dst++ = kHex[addr & 0x0Fu];
  *dst++ = ' ';
  *dst++ = (value & 1u) ? 'R' : 'W';
  *dst = '\0';
}

void decode_i2c(const capture_t *c, uint64_t t0, uint64_t t1,
                const decoder_cfg_t *cfg, annot_list_t *out) {
  const uint8_t sda = (uint8_t)(1u << (cfg->ch[0] & 7u));
  const uint8_t scl = (uint8_t)(1u << (cfg->ch[1] & 7u));
  if (sda == scl || t1 <= t0) {
    return; /* SDA and SCL must be different channels */
  }

  uint8_t prev = capture_mask_at(c, t0);
  uint64_t t = t0;

  bool in_frame = false;  /* between START and STOP */
  bool expect_addr = false;
  int bit_count = 0;
  uint32_t value = 0;
  uint64_t byte_t0 = 0;
  uint64_t last_rise = 0;
  uint64_t clock_ns = 0; /* shortest SCL period seen, for START/STOP width */

  uint64_t ts;
  uint8_t after;
  while (out->count < ANNOT_MAX &&
         capture_next_edge(c, t, t1, (uint8_t)(sda | scl), &ts, &after)) {
    const uint8_t changed = (uint8_t)(prev ^ after);

    if ((changed & sda) && (prev & scl) && (after & scl)) {
      /* SDA moved while the clock was held high. Markers are emitted with no
       * width and widened below, once the clock period is known. */
      const bool start = (after & sda) == 0u;
      const char text[2] = {start ? 'S' : 'P', '\0'};
      annot_add(out, ts, ts, ANNOT_FRAME, text);
      in_frame = start;
      expect_addr = start;
      bit_count = 0;
      value = 0;
    } else if ((changed & scl) && (after & scl)) {
      if (last_rise != 0ull) {
        const uint64_t period = ts - last_rise;
        if (clock_ns == 0ull || period < clock_ns) {
          clock_ns = period;
        }
      }
      last_rise = ts;

      if (in_frame) {
        const bool bit = (after & sda) != 0u;
        if (bit_count < 8) {
          if (bit_count == 0) {
            byte_t0 = ts;
          }
          value = (value << 1) | (bit ? 1u : 0u); /* MSB first */
          bit_count++;
        } else {
          /* 9th clock: the receiver drives SDA low to acknowledge. */
          char text[ANNOT_TEXT_MAX];
          uint8_t kind;
          if (bit) {
            kind = ANNOT_ERROR; /* NAK */
          } else {
            kind = expect_addr ? ANNOT_FRAME : ANNOT_DATA;
          }
          if (expect_addr) {
            format_addr(text, value);
          } else {
            annot_format_byte(text, value, 2);
          }
          annot_add(out, byte_t0, ts, kind, text);
          expect_addr = false;
          bit_count = 0;
          value = 0;
        }
      }
    }

    prev = after;
    t = ts + 1ull;
  }

  /* A marker is a single instant, so it would collapse to a 1 px tick. Grow it
   * into the idle half of the bus - backwards for START, forwards for STOP -
   * so the letter has room without covering the neighbouring byte. */
  if (clock_ns == 0ull) {
    clock_ns = (t1 - t0) / 100ull;
  }
  const uint64_t marker_ns = clock_ns * 2ull;
  for (int i = 0; i < out->count; i++) {
    annot_t *a = &out->items[i];
    if (a->t1 != a->t0) {
      continue;
    }
    if (a->text[0] == 'S') {
      a->t0 = (a->t0 > marker_ns) ? (a->t0 - marker_ns) : 0ull;
    } else {
      a->t1 = a->t0 + marker_ns;
    }
  }
}


/* ==== decoders/spi.c ============================================== */
/*
 * spi.c - synchronous serial (SPI) decoder, master-out line only.
 *
 *   ch[0] = SCK, ch[1] = MOSI, ch[2] = CS (>= SCOPE_CHANNELS: no chip select)
 *
 * Like I2C the bus carries its own clock, so there is no rate to configure -
 * only the mode and the bit order:
 *
 *   CPOL = mode >> 1   idle level of SCK
 *   CPHA = mode & 1    0 = sample on the leading edge, 1 = on the trailing one
 *
 * which collapses to "sample on a rising SCK when CPOL == CPHA" (modes 0 and 3)
 * and on a falling SCK otherwise (modes 1 and 2).
 *
 * With a chip select the decoder only counts clocks while CS is asserted (low)
 * and drops a partially received word when CS is released; without one it
 * decodes continuously and relies on the word boundary staying in step.
 */

void decode_spi(const capture_t *c, uint64_t t0, uint64_t t1,
                const decoder_cfg_t *cfg, annot_list_t *out) {
  const uint8_t sck = (uint8_t)(1u << (cfg->ch[0] & 7u));
  const uint8_t mosi = (uint8_t)(1u << (cfg->ch[1] & 7u));
  if (sck == mosi || t1 <= t0) {
    return; /* SCK and MOSI must be different channels */
  }
  const uint8_t cs =
      (cfg->ch[2] < SCOPE_CHANNELS) ? (uint8_t)(1u << cfg->ch[2]) : 0u;

  const bool sample_on_rise = ((cfg->spi_mode >> 1) & 1u) == (cfg->spi_mode & 1u);
  const bool msb_first = cfg->spi_msb != 0u;
  int bits = (cfg->bits >= 4u && cfg->bits <= 16u) ? (int)cfg->bits : 8;
  const int digits = (bits + 3) / 4;

  uint8_t prev = capture_mask_at(c, t0);
  uint64_t t = t0;

  bool selected = (cs == 0u) || ((prev & cs) == 0u);
  int bit_count = 0;
  uint32_t value = 0;
  uint64_t word_t0 = 0;

  uint64_t ts;
  uint8_t after;
  while (out->count < ANNOT_MAX &&
         capture_next_edge(c, t, t1, (uint8_t)(sck | cs), &ts, &after)) {
    const uint8_t changed = (uint8_t)(prev ^ after);

    if (cs != 0u && (changed & cs)) {
      selected = (after & cs) == 0u;
      bit_count = 0; /* a partial word is never valid across a CS edge */
      value = 0;
    } else if (selected && (changed & sck)) {
      const bool rising = (after & sck) != 0u;
      if (rising == sample_on_rise) {
        const bool bit = (after & mosi) != 0u;
        if (bit_count == 0) {
          word_t0 = ts;
          value = 0;
        }
        if (msb_first) {
          value = (value << 1) | (bit ? 1u : 0u);
        } else if (bit) {
          value |= 1u << bit_count;
        }
        if (++bit_count == bits) {
          char text[ANNOT_TEXT_MAX];
          annot_format_byte(text, value, digits);
          annot_add(out, word_t0, ts, ANNOT_DATA, text);
          bit_count = 0;
        }
      }
    }

    prev = after;
    t = ts + 1ull;
  }
}


/* ==== main.c ====================================================== */
/*
 * chip-logic-scope - 8 channel logic analyzer with an on-part display.
 *
 * Phase 1: live roll-mode waveforms.
 *   D0..D7 -> pin_watch(BOTH) -> transition ring -> per-column summary ->
 *   column-oriented waveform renderer -> framebuffer.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "wokwi-api.h"

/* ---------------------------------------------------------------- layout -- */

#define STATUS_H 16
#define PLOT_X 28
#define PLOT_W 450
#define DIV_W 50
#define DIV_COUNT (PLOT_W / DIV_W)
#define LANE_TOP 20
#define PLOT_H 272 /* 8 lanes x 34 px when every channel is shown */
#define PLOT_BOT (LANE_TOP + PLOT_H)
#define INFO_Y0 (PLOT_BOT + 3)
#define INFO_Y1 (INFO_Y0 + 10)
/* Height taken from the waveform area when a decoder is active. */
#define DECODE_H 26

/* ---------------------------------------------------------------- colors -- */

#define COL_BG RGBA(8, 10, 14)
#define COL_PANEL RGBA(22, 26, 34)
#define COL_GRID RGBA(40, 46, 58)
#define COL_FRAME RGBA(72, 82, 98)
#define COL_TEXT RGBA(205, 214, 228)
#define COL_TEXT_DIM RGBA(118, 128, 145)
#define COL_RUN RGBA(64, 220, 128)
#define COL_STOP RGBA(240, 92, 92)
#define COL_CURSOR RGBA(255, 208, 64)
#define COL_CURSOR_B RGBA(120, 255, 190)
#define COL_TRIG RGBA(120, 200, 255)
#define COL_GLITCH RGBA(255, 255, 255)

static const uint32_t kAnnotColors[ANNOT_KIND_COUNT] = {
    RGBA(70, 96, 132),  /* framing */
    RGBA(46, 96, 78),   /* data    */
    RGBA(150, 48, 48),  /* error   */
};

static const uint32_t kChannelColors[SCOPE_CHANNELS] = {
    RGBA(255, 96, 96),   /* D0 red     */
    RGBA(255, 168, 64),  /* D1 orange  */
    RGBA(240, 226, 90),  /* D2 yellow  */
    RGBA(112, 226, 112), /* D3 green   */
    RGBA(80, 220, 220),  /* D4 cyan    */
    RGBA(110, 160, 255), /* D5 blue    */
    RGBA(180, 130, 255), /* D6 violet  */
    RGBA(255, 120, 210), /* D7 magenta */
};

static const char *const kChannelNames[SCOPE_CHANNELS] = {
    "D0", "D1", "D2", "D3", "D4", "D5", "D6", "D7",
};

static const char *const kTriggerModeNames[TRIGGER_MODE_COUNT] = {
    "off", "edge", "level", "auto", "single",
};

/* font.h glyph 95/96, see GLYPH_UP / GLYPH_DOWN. */
static const char *const kTriggerEdgeGlyphs[TRIGGER_EDGE_COUNT] = {
    "\x01", "\x02", "\x01\x02",
};

/* -------------------------------------------------------------- timebase -- */

typedef struct {
  uint32_t ns_per_div;
  const char *label;
} timebase_t;

/* Every entry is a multiple of DIV_W, so ns_per_px is exact. */
static const timebase_t kTimebases[] = {
    {100, "100ns"},    {200, "200ns"},    {500, "500ns"},
    {1000, "1us"},     {2000, "2us"},     {5000, "5us"},
    {10000, "10us"},   {20000, "20us"},   {50000, "50us"},
    {100000, "100us"}, {200000, "200us"}, {500000, "500us"},
    {1000000, "1ms"},  {2000000, "2ms"},  {5000000, "5ms"},
    {10000000, "10ms"},
};
#define TIMEBASE_COUNT ((int)(sizeof(kTimebases) / sizeof(kTimebases[0])))
#define TIMEBASE_DEFAULT 9 /* 100us/div -> 900us window */

/* Index 0 means "use the uartBaud attribute"; the rest are the usual rates. */
static const uint32_t kBaudTable[] = {
    0, 300, 1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200, 230400,
};
#define BAUD_COUNT ((int)(sizeof(kBaudTable) / sizeof(kBaudTable[0])))

/* ------------------------------------------------------------------ menu -- */
/*
 * The Wokwi control panel has no layout, grouping or button support, so a long
 * list of sliders becomes unreadable. Two sliders drive a menu instead:
 * `settingIndex` picks a parameter and `settingValue` edits it, while the chip
 * screen shows the whole table. Values live in `g_setting` and are seeded from
 * the matching diagram.json attributes, so the diagram still configures
 * everything.
 *
 * `settingValue` spans 0..SETTING_RAW_MAX and is mapped onto each parameter's
 * own range. It only takes effect once it *moves*, otherwise picking a
 * parameter would immediately overwrite it with the slider's leftover position.
 */
#define SETTING_RAW_MAX 255u
#define MENU_HOLD_NS 3000000000ull /* overlay stays up this long after a change */

enum {
  SET_CURSOR_A = 0,
  SET_CURSOR_B,
  SET_TRIG_MODE,
  SET_TRIG_CH,
  SET_TRIG_EDGE,
  SET_PRE_TRIGGER,
  SET_CHAN_MASK,
  SET_MEASURE_CH,
  SET_DECODER,
  SET_DEC_CH_A,
  SET_DEC_CH_B,
  SET_DEC_CH_C,
  SET_BAUD,
  SET_SPI_MODE,
  SET_SPI_ORDER,
  SETTING_COUNT
};

typedef struct {
  const char *name;
  const char *attr;
  uint32_t max;
  uint32_t def;
  const char *const *labels; /* NULL -> render the number */
} setting_def_t;

static const char *const kEdgeNames[TRIGGER_EDGE_COUNT] = {
    "rise", "fall", "both",
};

static const char *const kDecoderLabels[DECODER_COUNT] = {
    "off", "UART", "I2C", "SPI",
};

/* Channel picker with an extra "none" slot, used for the optional SPI CS. */
static const char *const kChannelOrNone[SCOPE_CHANNELS + 1] = {
    "D0", "D1", "D2", "D3", "D4", "D5", "D6", "D7", "none",
};

static const char *const kSpiOrderNames[2] = {"LSB", "MSB"};

static const char *const kBaudLabels[BAUD_COUNT] = {
    "attr", "300", "1200", "2400",  "4800",  "9600",
    "19200", "38400", "57600", "115200", "230400",
};

static const setting_def_t kSettings[SETTING_COUNT] = {
    {"Cursor A", "cursorPos", 100, 25, NULL},
    {"Cursor B", "cursorPosB", 100, 75, NULL},
    {"Trig mode", "triggerMode", TRIGGER_MODE_COUNT - 1, TRIGGER_OFF,
     kTriggerModeNames},
    {"Trig ch", "triggerChannel", SCOPE_CHANNELS - 1, 0, kChannelNames},
    {"Trig edge", "triggerEdge", TRIGGER_EDGE_COUNT - 1, TRIGGER_RISING,
     kEdgeNames},
    {"Pre-trig", "preTriggerPercent", 100, 25, NULL},
    {"Chan mask", "channelMask", 255, 255, NULL},
    {"Measure", "measureChannel", SCOPE_CHANNELS - 1, 0, kChannelNames},
    {"Decoder", "decoder", DECODER_COUNT - 1, DECODER_NONE, kDecoderLabels},
    {"Dec ch A", "decoderCh0", SCOPE_CHANNELS - 1, 0, kChannelNames},
    {"Dec ch B", "decoderCh1", SCOPE_CHANNELS - 1, 1, kChannelNames},
    {"Dec ch C", "decoderCh2", SCOPE_CHANNELS, SCOPE_CHANNELS, kChannelOrNone},
    {"Baud", "uartBaudIndex", BAUD_COUNT - 1, 0, kBaudLabels},
    {"SPI mode", "spiMode", 3, 0, NULL},
    {"SPI order", "spiMsbFirst", 1, 1, kSpiOrderNames},
};

/* ----------------------------------------------------------------- state -- */

typedef struct {
  pin_t pins[SCOPE_CHANNELS];
  timer_t timer;
  uint32_t attr_timebase;
  uint32_t attr_running;
  uint32_t attr_setting_index;
  uint32_t attr_setting_value;
  uint32_t attr_uart_baud;
  uint32_t attr_uart_bits;
  uint32_t attr_uart_parity;
  uint32_t attr_uart_stop;
  uint64_t view_end_ns; /* right edge of the plot */
  bool running;
} scope_t;

/* Lane geometry, recomputed every frame from `channelMask`. */
typedef struct {
  int count;
  int height;
  int pad;
  int area_h; /* pixels available for waveform lanes */
  uint8_t ch[SCOPE_CHANNELS];
} lanes_t;

static scope_t g_scope;
static capture_t g_capture;
static trigger_t g_trigger;
static lanes_t g_lanes;
static annot_list_t g_annots;
static pin_watch_config_t g_watch[SCOPE_CHANNELS];

static uint8_t g_col_high[PLOT_W];
static uint8_t g_col_low[PLOT_W];
static uint8_t g_col_multi[PLOT_W];

/* Menu state: the live value of every setting plus the slider bookkeeping. */
static uint32_t g_setting[SETTING_COUNT];
static uint32_t g_setting_attr[SETTING_COUNT];
static uint32_t g_menu_index;
static uint32_t g_menu_raw;
static uint64_t g_menu_shown_ns;

/*
 * Settings edited with the sliders live in RAM only: the chip API can read
 * attributes but never write them back, so restarting the simulation reloads
 * every setting from diagram.json. Once the configuration settles it is
 * therefore printed to the Chips Console as a paste-ready `attrs` block, which
 * is the only way to make a setup permanent.
 */
static bool g_config_dirty;
static uint64_t g_config_change_ns;
static uint32_t g_last_timebase = 0xFFFFFFFFu;

/* How long the sliders must sit still (simulated) before the block is dumped. */
#define CONFIG_SETTLE_NS 500000000ull

static void config_touched(uint64_t now) {
  g_config_dirty = true;
  g_config_change_ns = now;
}

static void update_menu(uint64_t now) {
  uint32_t index = attr_read(g_scope.attr_setting_index);
  if (index >= (uint32_t)SETTING_COUNT) {
    index = (uint32_t)SETTING_COUNT - 1u;
  }
  const uint32_t raw = attr_read(g_scope.attr_setting_value);

  if (index != g_menu_index) {
    g_menu_index = index; /* catch-up: adopt the slider, do not apply it */
    g_menu_raw = raw;
    g_menu_shown_ns = now;
  } else if (raw != g_menu_raw) {
    g_menu_raw = raw;
    const uint32_t max = kSettings[index].max;
    g_setting[index] = (raw * max + SETTING_RAW_MAX / 2u) / SETTING_RAW_MAX;
    g_menu_shown_ns = now;
    config_touched(now);
  }
}

static void update_lanes(uint8_t mask, int area_h) {
  if (mask == 0u) {
    mask = 0xFFu; /* an empty mask would leave nothing to draw */
  }
  g_lanes.count = 0;
  for (int i = 0; i < SCOPE_CHANNELS; i++) {
    if (mask & (uint8_t)(1u << i)) {
      g_lanes.ch[g_lanes.count++] = (uint8_t)i;
    }
  }
  g_lanes.area_h = area_h;
  g_lanes.height = area_h / g_lanes.count;
  g_lanes.pad = g_lanes.height / 5;
  if (g_lanes.pad < 2) {
    g_lanes.pad = 2;
  }
  if (g_lanes.pad > 12) {
    g_lanes.pad = 12;
  }
}

/* ------------------------------------------------------------ formatting -- */

static char *str_append(char *p, const char *s) {
  while (*s != '\0') {
    *p++ = *s++;
  }
  return p;
}

static char *u32_append(char *p, uint32_t v) {
  char tmp[11];
  int n = 0;
  do {
    tmp[n++] = (char)('0' + (v % 10u));
    v /= 10u;
  } while (v != 0u);
  while (n > 0) {
    *p++ = tmp[--n];
  }
  return p;
}

/* Human readable duration with two decimals, e.g. "432.00us". */
static char *time_append(char *p, uint64_t ns) {
  const char *unit;
  uint64_t div;
  if (ns < 1000ull) {
    unit = "ns";
    div = 1ull;
  } else if (ns < 1000000ull) {
    unit = "us";
    div = 1000ull;
  } else if (ns < 1000000000ull) {
    unit = "ms";
    div = 1000000ull;
  } else {
    unit = "s";
    div = 1000000000ull;
  }
  p = u32_append(p, (uint32_t)(ns / div));
  if (div > 1ull) {
    const uint32_t frac = (uint32_t)(((ns % div) * 100ull) / div);
    *p++ = '.';
    *p++ = (char)('0' + frac / 10u);
    *p++ = (char)('0' + frac % 10u);
  }
  return str_append(p, unit);
}

/* Frequency with three decimals, from a value in milli-hertz. */
static char *freq_append(char *p, uint64_t millihz) {
  if (millihz == 0ull) {
    return str_append(p, "--");
  }
  const char *unit;
  uint64_t div;
  if (millihz >= 1000000000ull) {
    unit = "MHz";
    div = 1000000000ull;
  } else if (millihz >= 1000000ull) {
    unit = "kHz";
    div = 1000000ull;
  } else {
    unit = "Hz";
    div = 1000ull;
  }
  p = u32_append(p, (uint32_t)(millihz / div));
  const uint32_t frac = (uint32_t)(((millihz % div) * 1000ull) / div);
  *p++ = '.';
  *p++ = (char)('0' + frac / 100u);
  *p++ = (char)('0' + (frac / 10u) % 10u);
  *p++ = (char)('0' + frac % 10u);
  return str_append(p, unit);
}

/* 1/dt as milli-hertz, for the cursor and period readouts. */
static uint64_t millihz_from_ns(uint64_t ns) {
  return (ns == 0ull) ? 0ull : (1000000000000ull / ns);
}

/* --------------------------------------------------------------- capture -- */

/* Hot path: one compare and (on a real change) one ring append. */
static void on_pin_change(void *user_data, pin_t pin, uint32_t value) {
  (void)pin;
  const uint8_t bit = (uint8_t)(uintptr_t)user_data;
  const uint8_t prev = g_capture.mask;
  const uint8_t next = value ? (uint8_t)(prev | bit) : (uint8_t)(prev & ~bit);
  if (next != prev) {
    const uint64_t now = get_sim_nanos();
    capture_push(&g_capture, now, next);
    trigger_on_change(&g_trigger, now, prev, next);
  }
}

/* ---------------------------------------------------------------- chrome -- */

static void draw_status_bar(const timebase_t *tb, uint64_t window_ns,
                            uint8_t decoder_id, const decoder_cfg_t *cfg) {
  fb_fill_rect(0, 0, SCREEN_W, STATUS_H, COL_PANEL);
  fb_hline(0, STATUS_H - 1, SCREEN_W, COL_FRAME);

  const char *state = "RUN";
  uint32_t state_color = COL_RUN;
  if (!g_scope.running) {
    state = "STOP";
    state_color = COL_STOP;
  } else if (g_trigger.mode == TRIGGER_OFF) {
    state = "ROLL";
  } else if (g_trigger.state == TRIGGER_ARMED) {
    state = "ARM";
    state_color = COL_CURSOR;
  } else if (g_trigger.state == TRIGGER_HOLD) {
    state = "HOLD";
    state_color = COL_TRIG;
  }
  const int x = fb_text(4, 4, state, state_color, 1);

  char line[128];
  char *p = line;
  p = str_append(p, "  ");
  p = str_append(p, tb->label);
  p = str_append(p, "/div  win ");
  p = time_append(p, window_ns);
  if (g_trigger.mode != TRIGGER_OFF) {
    p = str_append(p, "  T ");
    p = str_append(p, kChannelNames[g_trigger.channel]);
    p = str_append(p, kTriggerEdgeGlyphs[g_trigger.edge]);
    *p++ = ' ';
    p = str_append(p, kTriggerModeNames[g_trigger.mode]);
    *p++ = ' ';
    p = u32_append(p, g_trigger.pre_percent);
    *p++ = '%';
  }
  p = str_append(p, "  ev ");
  p = u32_append(p, g_capture.total);
  p = str_append(p, "  buf ");
  p = u32_append(p, g_capture.count);
  if (decoder_id != DECODER_NONE) {
    p = str_append(p, "  ");
    p = str_append(p, decoder_name(decoder_id));
    *p++ = ' ';
    p = str_append(p, kChannelNames[cfg->ch[0]]);
    if (decoder_id == DECODER_UART) {
      *p++ = ' ';
      p = u32_append(p, cfg->baud);
    } else {
      *p++ = '/';
      p = str_append(p, kChannelNames[cfg->ch[1]]);
      if (decoder_id == DECODER_SPI) {
        if (cfg->ch[2] < SCOPE_CHANNELS) {
          *p++ = '/';
          p = str_append(p, kChannelNames[cfg->ch[2]]);
        }
        p = str_append(p, " m");
        p = u32_append(p, cfg->spi_mode);
      }
    }
    p = str_append(p, " x");
    p = u32_append(p, (uint32_t)g_annots.count);
  }
  *p = '\0';
  fb_text(x + 4, 4, line, COL_TEXT, 1);
}

static void draw_grid(void) {
  fb_rect(PLOT_X - 1, LANE_TOP - 1, PLOT_W + 2, PLOT_H + 2, COL_FRAME);

  for (int d = 1; d < DIV_COUNT; d++) {
    fb_vline_dotted(PLOT_X + d * DIV_W, LANE_TOP, PLOT_H, COL_GRID, 3);
  }
  for (int l = 1; l < g_lanes.count; l++) {
    fb_hline_dotted(PLOT_X, LANE_TOP + l * g_lanes.height, PLOT_W, COL_GRID, 3);
  }
}

static void draw_gutter(uint8_t live_mask) {
  for (int l = 0; l < g_lanes.count; l++) {
    const int ch = g_lanes.ch[l];
    const int lane_y = LANE_TOP + l * g_lanes.height;
    const int mid = lane_y + g_lanes.height / 2;
    const uint32_t color = kChannelColors[ch];
    const bool high = (live_mask & (1u << ch)) != 0;

    fb_fill_rect(PLOT_X - 5, lane_y + 3, 3, g_lanes.height - 6, color);
    fb_text(3, mid - 8, kChannelNames[ch], color, 1);
    fb_text(9, mid + 1, high ? "1" : "0", high ? color : COL_TEXT_DIM, 1);
  }
}

static void draw_waveforms(void) {
  for (int l = 0; l < g_lanes.count; l++) {
    const int ch = g_lanes.ch[l];
    const int lane_y = LANE_TOP + l * g_lanes.height;
    const int y_hi = lane_y + g_lanes.pad;
    const int y_lo = lane_y + g_lanes.height - g_lanes.pad - 1;
    const uint8_t bit = (uint8_t)(1u << ch);
    const uint32_t color = kChannelColors[ch];

    for (int x = 0; x < PLOT_W; x++) {
      const bool high = (g_col_high[x] & bit) != 0;
      const bool low = (g_col_low[x] & bit) != 0;
      if (high && low) {
        /* One or more transitions fall inside this column. Two or more means
         * the column is aliased, which is drawn in the glitch color. */
        const bool glitch = (g_col_multi[x] & bit) != 0;
        fb_vline(PLOT_X + x, y_hi, y_lo - y_hi + 1, glitch ? COL_GLITCH : color);
      } else if (high) {
        fb_pixel(PLOT_X + x, y_hi, color);
      } else {
        fb_pixel(PLOT_X + x, y_lo, color);
      }
    }
  }
}

/* Vertical marker at the trigger point, i.e. `preTriggerPercent` of the plot. */
static void draw_trigger_marker(int col) {
  fb_vline_dotted(PLOT_X + col, LANE_TOP, PLOT_H, COL_TRIG, 4);
  fb_text(PLOT_X + col + 2, LANE_TOP + 1, "T", COL_TRIG, 1);
}

static void draw_cursors(int col_a, int col_b, uint64_t ns_per_px) {
  fb_vline_dotted(PLOT_X + col_a, LANE_TOP, PLOT_H, COL_CURSOR, 2);
  fb_vline_dotted(PLOT_X + col_b, LANE_TOP, PLOT_H, COL_CURSOR_B, 3);
  fb_text(PLOT_X + col_a + 2, LANE_TOP + 1, "A", COL_CURSOR, 1);
  fb_text(PLOT_X + col_b + 2, PLOT_BOT - 9, "B", COL_CURSOR_B, 1);

  const int delta_px = (col_b > col_a) ? (col_b - col_a) : (col_a - col_b);
  const uint64_t delta_ns = (uint64_t)delta_px * ns_per_px;

  char line[128];
  char *p = line;
  p = str_append(p, "A ");
  p = time_append(p, (uint64_t)col_a * ns_per_px);
  p = str_append(p, "  B ");
  p = time_append(p, (uint64_t)col_b * ns_per_px);
  p = str_append(p, "  dt ");
  p = time_append(p, delta_ns);
  p = str_append(p, "  1/dt ");
  p = freq_append(p, millihz_from_ns(delta_ns));
  p = str_append(p, "  D7..D0 ");

  const uint8_t high = g_col_high[col_a];
  const uint8_t low = g_col_low[col_a];
  for (int i = SCOPE_CHANNELS - 1; i >= 0; i--) {
    const uint8_t bit = (uint8_t)(1u << i);
    const bool h = (high & bit) != 0;
    const bool l = (low & bit) != 0;
    *p++ = (h && l) ? 'X' : (h ? '1' : '0');
  }
  *p = '\0';
  fb_text(4, INFO_Y0, line, COL_TEXT_DIM, 1);
}

/* Auto-measurements for one channel over the whole visible window. */
static void draw_measurements(uint8_t channel, const measure_t *m) {
  char line[128];
  char *p = line;
  p = str_append(p, kChannelNames[channel]);
  p = str_append(p, "  f ");
  p = freq_append(p, millihz_from_ns(m->period_ns));
  p = str_append(p, "  T ");
  p = time_append(p, m->period_ns);
  p = str_append(p, "  duty ");
  p = u32_append(p, m->duty_tenths / 10u);
  *p++ = '.';
  *p++ = (char)('0' + m->duty_tenths % 10u);
  p = str_append(p, "%  min ");
  p = time_append(p, m->min_pulse_ns);
  p = str_append(p, "  ed ");
  p = u32_append(p, m->edges);
  *p = '\0';
  fb_text(4, INFO_Y1, line, kChannelColors[channel], 1);
}

/* Protocol annotations, drawn as boxes under the waveform lanes. */
static void draw_decode_lane(uint8_t decoder_id, uint64_t view_start,
                             uint64_t ns_per_px) {
  const int top = LANE_TOP + g_lanes.area_h;
  const int box_y = top + 5;
  const int box_h = DECODE_H - 9;

  fb_hline(PLOT_X, top, PLOT_W, COL_FRAME);
  fb_text(2, top + DECODE_H / 2 - 3, decoder_name(decoder_id), COL_TEXT_DIM, 1);

  for (int i = 0; i < g_annots.count; i++) {
    const annot_t *a = &g_annots.items[i];
    if (a->t1 <= view_start) {
      continue;
    }
    const int x0 =
        (a->t0 > view_start) ? (int)((a->t0 - view_start) / ns_per_px) : 0;
    if (x0 >= PLOT_W) {
      continue;
    }
    int x1 = (int)((a->t1 - view_start) / ns_per_px);
    if (x1 > PLOT_W - 1) {
      x1 = PLOT_W - 1;
    }
    const int w = x1 - x0 + 1;
    const uint32_t fill =
        kAnnotColors[(a->kind < ANNOT_KIND_COUNT) ? a->kind : ANNOT_FRAME];

    if (w < 3) {
      fb_vline(PLOT_X + x0, box_y, box_h, fill); /* too narrow for a box */
      continue;
    }
    fb_fill_rect(PLOT_X + x0, box_y, w, box_h, fill);
    fb_rect(PLOT_X + x0, box_y, w, box_h, COL_FRAME);

    /* Longest prefix of the label that still fits: "48 'H'" -> "48" -> "4". */
    char text[ANNOT_TEXT_MAX];
    int n = 0;
    while (a->text[n] != '\0' && n < ANNOT_TEXT_MAX - 1) {
      text[n] = a->text[n];
      n++;
    }
    while (n > 0 && fb_text_width_n(text, n, 1) > w - 4) {
      n--;
    }
    if (n > 0) {
      text[n] = '\0';
      const int tw = fb_text_width(text, 1);
      fb_text(PLOT_X + x0 + (w - tw) / 2, box_y + (box_h - 7) / 2, text,
              COL_TEXT, 1);
    }
  }
}

/* Settings overlay, shown for a few seconds after either menu slider moves. */
#define MENU_COLS 2
#define MENU_ROWS ((SETTING_COUNT + MENU_COLS - 1) / MENU_COLS)
#define MENU_CELL_W 130
#define MENU_ROW_H 11

static void draw_menu(void) {
  const int w = MENU_COLS * MENU_CELL_W + 10;
  const int h = MENU_ROWS * MENU_ROW_H + 30;
  const int x = PLOT_X + (PLOT_W - w) / 2;
  const int y = LANE_TOP + 24;

  fb_fill_rect(x, y, w, h, COL_PANEL);
  fb_rect(x, y, w, h, COL_FRAME);
  fb_text(x + 5, y + 4, "SETTINGS  (Setting picks, Value edits)",
          COL_TEXT_DIM, 1);
  fb_hline(x + 1, y + 14, w - 2, COL_FRAME);

  for (int i = 0; i < SETTING_COUNT; i++) {
    const setting_def_t *s = &kSettings[i];
    const bool selected = (i == (int)g_menu_index);
    const int cx = x + 5 + (i / MENU_ROWS) * MENU_CELL_W;
    const int cy = y + 18 + (i % MENU_ROWS) * MENU_ROW_H;

    if (selected) {
      fb_fill_rect(cx - 2, cy - 2, MENU_CELL_W - 2, MENU_ROW_H, COL_FRAME);
    }
    const uint32_t color = selected ? COL_TEXT : COL_TEXT_DIM;
    fb_text(cx, cy, s->name, color, 1);

    char value[16];
    char *p = value;
    if (s->labels != NULL) {
      p = str_append(p, s->labels[g_setting[i]]);
    } else {
      p = u32_append(p, g_setting[i]);
    }
    *p = '\0';
    const int vw = fb_text_width(value, 1);
    fb_text(cx + MENU_CELL_W - 10 - vw, cy, value, color, 1);
  }

  fb_hline(x + 1, y + h - 13, w - 2, COL_FRAME);
  fb_text(x + 5, y + h - 10, "lost on restart: see the Chips Console",
          COL_TEXT_DIM, 1);
}

/* Dumps the whole configuration as a diagram.json "attrs" block. */
static void print_config(uint32_t timebase_index) {
  char line[640];
  char *p = line;
  p = str_append(p, "  \"attrs\": { \"timebaseIndex\": \"");
  p = u32_append(p, timebase_index);
  *p++ = '"';
  for (int i = 0; i < SETTING_COUNT; i++) {
    p = str_append(p, ", \"");
    p = str_append(p, kSettings[i].attr);
    p = str_append(p, "\": \"");
    p = u32_append(p, g_setting[i]);
    *p++ = '"';
  }
  p = str_append(p, " }");
  *p = '\0';
  printf(
      "[logic-scope] settings reset on restart; paste this into the part in "
      "diagram.json to keep them:\n%s\n",
      line);
  fflush(stdout);
}

/* ---------------------------------------------------------------- sweep -- */
/* AUTO falls back to rolling after this much idle time (simulated). */
static uint64_t auto_timeout_ns(uint64_t window_ns) {
  const uint64_t t = window_ns * 2ull;
  return (t < 100000000ull) ? 100000000ull : t; /* at least 100 ms */
}

/* Re-reads the trigger settings; any change re-arms on the next frame. */
static void update_trigger_config(void) {
  const uint32_t mode = g_setting[SET_TRIG_MODE];
  const uint32_t channel = g_setting[SET_TRIG_CH];
  const uint32_t edge = g_setting[SET_TRIG_EDGE];

  if (mode != g_trigger.mode || channel != g_trigger.channel ||
      edge != g_trigger.edge) {
    g_trigger.mode = (uint8_t)mode;
    g_trigger.channel = (uint8_t)channel;
    g_trigger.edge = (uint8_t)edge;
    trigger_idle(&g_trigger);
  }
  g_trigger.pre_percent = (uint8_t)g_setting[SET_PRE_TRIGGER];
}

/*
 * Decides which time window the next frame shows. A triggered sweep keeps the
 * previous (complete) frame on screen until the new post-trigger window has
 * been captured, so the display never shows a half-filled sweep.
 */
static void update_sweep(uint64_t now, uint64_t window_ns) {
  if (!g_scope.running) {
    trigger_idle(&g_trigger); /* frame stays frozen at view_end_ns */
    return;
  }
  if (g_trigger.mode == TRIGGER_OFF) {
    trigger_idle(&g_trigger);
    g_scope.view_end_ns = now;
    return;
  }

  const uint64_t post_ns =
      (window_ns * (100u - (uint32_t)g_trigger.pre_percent)) / 100u;

  switch (g_trigger.state) {
    case TRIGGER_IDLE:
      trigger_arm(&g_trigger, now, g_capture.mask);
      break;

    case TRIGGER_ARMED:
      if (g_trigger.mode == TRIGGER_AUTO &&
          now - g_trigger.armed_ns >= auto_timeout_ns(window_ns)) {
        g_scope.view_end_ns = now; /* free-run until a real trigger arrives */
      }
      break;

    case TRIGGER_FILLING:
      if (now >= g_trigger.fire_ns + post_ns) {
        g_scope.view_end_ns = g_trigger.fire_ns + post_ns;
        g_trigger.state = TRIGGER_HOLD;
      }
      break;

    case TRIGGER_HOLD:
      if (g_trigger.mode != TRIGGER_SINGLE) {
        trigger_arm(&g_trigger, now, g_capture.mask);
      }
      break;

    default:
      break;
  }
}

/* ---------------------------------------------------------------- render -- */

/* Maps a 0..100 cursor setting to a plot column. */
static int cursor_column(uint32_t percent) {
  if (percent > 100u) {
    percent = 100u;
  }
  return (int)((percent * (uint32_t)(PLOT_W - 1)) / 100u);
}

static void render(void *user_data) {
  (void)user_data;

  const uint64_t now = get_sim_nanos();
  update_menu(now);

  uint32_t tb_index = attr_read(g_scope.attr_timebase);
  if (tb_index >= (uint32_t)TIMEBASE_COUNT) {
    tb_index = (uint32_t)TIMEBASE_COUNT - 1u;
  }
  if (tb_index != g_last_timebase) {
    if (g_last_timebase != 0xFFFFFFFFu) {
      config_touched(now);
    }
    g_last_timebase = tb_index;
  }
  const timebase_t *tb = &kTimebases[tb_index];
  const uint64_t ns_per_px = tb->ns_per_div / DIV_W;
  const uint64_t window_ns = ns_per_px * (uint64_t)PLOT_W;

  const bool was_running = g_scope.running;
  g_scope.running = attr_read(g_scope.attr_running) != 0u;
  const bool just_stopped = was_running && !g_scope.running;
  update_trigger_config();
  update_sweep(now, window_ns);

  const uint32_t decoder_id = g_setting[SET_DECODER];
  const int wave_h =
      (decoder_id != DECODER_NONE) ? (PLOT_H - DECODE_H) : PLOT_H;
  update_lanes((uint8_t)g_setting[SET_CHAN_MASK], wave_h);

  const uint64_t view_end = g_scope.view_end_ns;
  const uint64_t view_start = (view_end > window_ns) ? (view_end - window_ns) : 0;

  decoder_cfg_t cfg;
  cfg.ch[0] = (uint8_t)g_setting[SET_DEC_CH_A];
  cfg.ch[1] = (uint8_t)g_setting[SET_DEC_CH_B];
  cfg.ch[2] = (uint8_t)g_setting[SET_DEC_CH_C];
  cfg.ch[3] = 3;
  cfg.baud = attr_read(g_scope.attr_uart_baud);
  const uint32_t baud_index = g_setting[SET_BAUD];
  if (baud_index != 0u) {
    cfg.baud = kBaudTable[baud_index];
  }
  cfg.bits = (uint8_t)attr_read(g_scope.attr_uart_bits);
  cfg.parity = (uint8_t)attr_read(g_scope.attr_uart_parity);
  cfg.stop = (uint8_t)attr_read(g_scope.attr_uart_stop);
  cfg.spi_mode = (uint8_t)g_setting[SET_SPI_MODE];
  cfg.spi_msb = (uint8_t)g_setting[SET_SPI_ORDER];

  capture_columns(&g_capture, view_start, ns_per_px, PLOT_W, g_col_high,
                  g_col_low, g_col_multi);

  if (decoder_id != DECODER_NONE) {
    decoder_run((uint8_t)decoder_id, &g_capture, view_start, view_end, &cfg,
                &g_annots);
  } else {
    annot_reset(&g_annots);
  }

  const uint32_t measure_ch = g_setting[SET_MEASURE_CH];
  measure_t meas;
  capture_measure(&g_capture, view_start, view_end, (uint8_t)measure_ch, &meas);

  const int col_a = cursor_column(g_setting[SET_CURSOR_A]);
  const int col_b = cursor_column(g_setting[SET_CURSOR_B]);
  fb_clear(COL_BG);
  draw_status_bar(tb, window_ns, (uint8_t)decoder_id, &cfg);
  draw_grid();
  draw_gutter(g_capture.mask);
  draw_waveforms();
  if (decoder_id != DECODER_NONE) {
    draw_decode_lane((uint8_t)decoder_id, view_start, ns_per_px);
  }
  if (g_trigger.mode != TRIGGER_OFF) {
    draw_trigger_marker((int)(((uint32_t)g_trigger.pre_percent *
                               (uint32_t)(PLOT_W - 1)) /
                              100u));
  }
  draw_cursors(col_a, col_b, ns_per_px);
  draw_measurements((uint8_t)measure_ch, &meas);
  if (now - g_menu_shown_ns < MENU_HOLD_NS) {
    draw_menu();
  }
  render_flush();

  /* Once the sliders have been still for a while, offer the config for reuse. */
  if (g_config_dirty &&
      (just_stopped || now - g_config_change_ns >= CONFIG_SETTLE_NS)) {
    g_config_dirty = false;
    print_config(tb_index);
  }
}

/* ------------------------------------------------------------------ init -- */

void chip_init(void) {
  /* Some wasi-libc builds fully buffer stdout; a chip never exits, so an
   * unflushed buffer would simply never reach the Chips Console. */
  setvbuf(stdout, NULL, _IONBF, 0);

  if (!render_init()) {
    return;
  }

  uint8_t mask = 0;
  for (int i = 0; i < SCOPE_CHANNELS; i++) {
    g_scope.pins[i] = pin_init(kChannelNames[i], INPUT);
    if (pin_read(g_scope.pins[i])) {
      mask |= (uint8_t)(1u << i);
    }
  }
  if (!capture_init(&g_capture, mask)) {
    printf("[logic-scope] out of memory: cannot allocate the event ring\n");
    return;
  }

  for (int i = 0; i < SCOPE_CHANNELS; i++) {
    g_watch[i].user_data = (void *)(uintptr_t)(1u << i);
    g_watch[i].edge = BOTH;
    g_watch[i].pin_change = on_pin_change;
    if (!pin_watch(g_scope.pins[i], &g_watch[i])) {
      printf("[logic-scope] pin_watch failed for %s\n", kChannelNames[i]);
    }
  }

  g_scope.attr_timebase = attr_init("timebaseIndex", TIMEBASE_DEFAULT);
  g_scope.attr_running = attr_init("running", 1);
  g_scope.attr_setting_index = attr_init("settingIndex", 0);
  g_scope.attr_setting_value = attr_init("settingValue", 0);
  g_scope.attr_uart_baud = attr_init("uartBaud", 115200);
  g_scope.attr_uart_bits = attr_init("uartBits", 8);
  g_scope.attr_uart_parity = attr_init("uartParity", 0);
  g_scope.attr_uart_stop = attr_init("uartStop", 1);
  const uint32_t attr_refresh = attr_init("refreshHz", 20);

  /* Menu settings keep their own attributes, so diagram.json still works. */
  for (int i = 0; i < SETTING_COUNT; i++) {
    g_setting_attr[i] = attr_init(kSettings[i].attr, kSettings[i].def);
  }
  for (int i = 0; i < SETTING_COUNT; i++) {
    const uint32_t v = attr_read(g_setting_attr[i]);
    g_setting[i] = (v > kSettings[i].max) ? kSettings[i].max : v;
  }
  g_menu_index = attr_read(g_scope.attr_setting_index);
  if (g_menu_index >= (uint32_t)SETTING_COUNT) {
    g_menu_index = (uint32_t)SETTING_COUNT - 1u;
  }
  g_menu_raw = attr_read(g_scope.attr_setting_value);

  g_scope.view_end_ns = get_sim_nanos();
  g_trigger.pre_percent = (uint8_t)g_setting[SET_PRE_TRIGGER];
  update_lanes(0xFF, PLOT_H);

  uint32_t refresh_hz = attr_read(attr_refresh);
  if (refresh_hz < 1u) {
    refresh_hz = 1u;
  }
  if (refresh_hz > 120u) {
    refresh_hz = 120u;
  }

  const timer_config_t cfg = {.user_data = &g_scope, .callback = render};
  g_scope.timer = timer_init(&cfg);
  timer_start(g_scope.timer, 1000000u / refresh_hz, true);

  printf("[logic-scope] ready: %dx%d, %d ch, ring %d events, %u Hz refresh\n",
         SCREEN_W, SCREEN_H, SCOPE_CHANNELS, SCOPE_MAX_EVENTS, refresh_hz);
  printf(
      "[logic-scope] slider settings are not saved: after every change the "
      "chip prints an \"attrs\" block here - paste it into this part in "
      "diagram.json to make it the startup setup\n");
}

