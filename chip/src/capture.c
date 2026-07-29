#include "capture.h"

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
