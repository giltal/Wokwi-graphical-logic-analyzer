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
#include "decoder.h"

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
