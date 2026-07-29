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
