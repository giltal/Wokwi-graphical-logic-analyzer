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

#include "capture.h"
#include "decoders/decoder.h"
#include "render.h"
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
 * every setting from diagram.json. Once the configuration settles the whole
 * menu is therefore packed into a couple of plain integers and printed to the
 * Chips Console; pasting them back as `setup0`/`setup1` attributes is the only
 * way to make a setup permanent.
 *
 * Field widths come from `kSettings[].max`, so adding a row to that table
 * extends the layout by itself. Bump SETUP_WORDS when it no longer fits -
 * chip_init() says so on the console.
 */
#define SETUP_WORDS 2
#define SETUP_BITS (SETUP_WORDS * 32u)

static const char *const kSetupAttrNames[] = {
    "setup0", "setup1", "setup2", "setup3",
};

static bool g_config_dirty;
static uint64_t g_config_change_ns;
static uint32_t g_setup_attr[SETUP_WORDS];
static uint32_t g_timebase = TIMEBASE_DEFAULT; /* effective time/div */
static uint32_t g_timebase_raw;                /* last seen slider position */

/* How long the sliders must sit still (simulated) before the block is dumped. */
#define CONFIG_SETTLE_NS 500000000ull

static void config_touched(uint64_t now) {
  g_config_dirty = true;
  g_config_change_ns = now;
}

/* ---------------------------------------------------------- setup words -- */

static uint32_t bits_for(uint32_t max) {
  uint32_t n = 1u;
  while ((max >> n) != 0u) {
    n++;
  }
  return n;
}

typedef struct {
  uint32_t word[SETUP_WORDS];
  uint32_t bit;
} setup_bits_t;

static void setup_put(setup_bits_t *s, uint32_t value, uint32_t width) {
  for (uint32_t i = 0; i < width && s->bit < SETUP_BITS; i++, s->bit++) {
    if (value & (1u << i)) {
      s->word[s->bit >> 5] |= 1u << (s->bit & 31u);
    }
  }
}

static uint32_t setup_get(setup_bits_t *s, uint32_t width) {
  uint32_t v = 0;
  for (uint32_t i = 0; i < width && s->bit < SETUP_BITS; i++, s->bit++) {
    if (s->word[s->bit >> 5] & (1u << (s->bit & 31u))) {
      v |= 1u << i;
    }
  }
  return v;
}

static uint32_t setup_layout_bits(void) {
  uint32_t bits = bits_for((uint32_t)TIMEBASE_COUNT - 1u);
  for (int i = 0; i < SETTING_COUNT; i++) {
    bits += bits_for(kSettings[i].max);
  }
  return bits;
}

static void setup_pack(uint32_t timebase_index, uint32_t *out) {
  setup_bits_t s;
  for (int i = 0; i < SETUP_WORDS; i++) {
    s.word[i] = 0;
  }
  s.bit = 0;
  setup_put(&s, timebase_index, bits_for((uint32_t)TIMEBASE_COUNT - 1u));
  for (int i = 0; i < SETTING_COUNT; i++) {
    setup_put(&s, g_setting[i], bits_for(kSettings[i].max));
  }
  for (int i = 0; i < SETUP_WORDS; i++) {
    out[i] = s.word[i];
  }
}

/* All-zero words mean "not set": the individual attributes then stay in force. */
static bool setup_unpack(const uint32_t *in, uint32_t *timebase_index) {
  setup_bits_t s;
  uint32_t any = 0;
  for (int i = 0; i < SETUP_WORDS; i++) {
    s.word[i] = in[i];
    any |= in[i];
  }
  if (any == 0u) {
    return false;
  }
  s.bit = 0;
  const uint32_t tb = setup_get(&s, bits_for((uint32_t)TIMEBASE_COUNT - 1u));
  *timebase_index =
      (tb >= (uint32_t)TIMEBASE_COUNT) ? (uint32_t)TIMEBASE_COUNT - 1u : tb;
  for (int i = 0; i < SETTING_COUNT; i++) {
    const uint32_t v = setup_get(&s, bits_for(kSettings[i].max));
    g_setting[i] = (v > kSettings[i].max) ? kSettings[i].max : v;
  }
  return true;
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

/* Dumps the whole configuration as the handful of integers that restore it. */
static void print_config(uint32_t timebase_index) {
  uint32_t words[SETUP_WORDS];
  setup_pack(timebase_index, words);

  char line[128];
  char *p = line;
  for (int i = 0; i < SETUP_WORDS; i++) {
    if (i != 0) {
      p = str_append(p, ", ");
    }
    *p++ = '"';
    p = str_append(p, kSetupAttrNames[i]);
    p = str_append(p, "\": \"");
    p = u32_append(p, words[i]);
    *p++ = '"';
  }
  *p = '\0';
  printf(
      "[logic-scope] setup changed; to keep it, add these attrs to this part "
      "in diagram.json:\n  %s\n",
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

  /* Catch-up, as for the menu: the packed setup holds until the slider moves. */
  const uint32_t tb_raw = attr_read(g_scope.attr_timebase);
  if (tb_raw != g_timebase_raw) {
    g_timebase_raw = tb_raw;
    g_timebase = tb_raw;
    config_touched(now);
  }
  uint32_t tb_index = g_timebase;
  if (tb_index >= (uint32_t)TIMEBASE_COUNT) {
    tb_index = (uint32_t)TIMEBASE_COUNT - 1u;
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

  /* `setup0`/`setup1` carry the same menu in packed form and win when present. */
  uint32_t setup[SETUP_WORDS];
  for (int i = 0; i < SETUP_WORDS; i++) {
    g_setup_attr[i] = attr_init(kSetupAttrNames[i], 0);
  }
  for (int i = 0; i < SETUP_WORDS; i++) {
    setup[i] = attr_read(g_setup_attr[i]);
  }
  g_timebase_raw = attr_read(g_scope.attr_timebase);
  g_timebase = g_timebase_raw;
  const bool packed = setup_unpack(setup, &g_timebase);

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
      "[logic-scope] setup loaded from %s; slider edits are not saved, but the "
      "chip prints a \"setup0\"/\"setup1\" pair here after every change - put "
      "it in this part's attrs to make it the startup setup\n",
      packed ? "setup0/setup1" : "the individual attributes");
  if (setup_layout_bits() > SETUP_BITS) {
    printf("[logic-scope] setup layout needs %u bits: bump SETUP_WORDS\n",
           setup_layout_bits());
  }
}
