#include "decoder.h"

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
