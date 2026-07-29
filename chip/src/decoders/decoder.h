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

#include "../capture.h"

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
