/*
 * uart.c - asynchronous serial decoder.
 *
 * Idle line is high. Every falling edge is a candidate start bit; data bits are
 * sampled in the middle of each bit cell (LSB first), then the optional parity
 * bit and the stop bit(s) are checked. A frame that would run past the end of
 * the visible window is left undecoded until it is fully captured.
 */
#include "decoder.h"

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
