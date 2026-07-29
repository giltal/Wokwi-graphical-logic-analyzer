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
#include "decoder.h"

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
