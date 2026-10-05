/**
 * @file    protocol.h
 * @brief   STM32 → ESP32 UART frame (NMEA-style text format, with checksum).
 *
 *  $MM,<seq>,<temp_d>,<hum_d>,<vib_mg>,<peak_mg>,<curr_ma>,<flags>*<CS>\r\n
 *
 *   seq      : counter 0..65535 (detects lost frames)
 *   temp_d   : temperature in tenths of °C (e.g. 253 = 25.3 °C), signed
 *   hum_d    : humidity in tenths of %
 *   vib_mg   : RMS vibration in milli-g
 *   peak_mg  : peak vibration in milli-g
 *   curr_ma  : current in mA
 *   flags    : bit0 DHT ok | bit1 MPU ok | bit2 simulated fault
 *              bits 4-5 overall level (0 normal, 1 warning, 2 critical)
 *   CS       : XOR of all bytes between '$' and '*', in hexadecimal (2 chars)
 *
 *  Why integers? printf("%f") is disabled by default in newlib-nano
 *  (saves ~10 KB of flash) and integers decode unambiguously. Example:
 *    $MM,42,253,451,85,140,1620,3*20\r\n
 *
 *  This module is plain C (no HAL dependency) → tested on a PC (test/).
 */
#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PROTO_MAX_FRAME   96

#define PROTO_FLAG_DHT_OK   (1u << 0)
#define PROTO_FLAG_MPU_OK   (1u << 1)
#define PROTO_FLAG_FAULT    (1u << 2)
#define PROTO_LEVEL_SHIFT   4u
#define PROTO_LEVEL_MASK    (3u << PROTO_LEVEL_SHIFT)

typedef struct {
  uint16_t seq;
  int16_t  temp_d;
  uint16_t hum_d;
  uint16_t vib_mg;
  uint16_t peak_mg;
  uint16_t curr_ma;
  uint8_t  flags;
} proto_frame_t;

typedef enum {
  PROTO_WAIT_START = 0,
  PROTO_IN_BODY,
  PROTO_IN_CS,
} proto_state_t;

typedef struct {
  proto_state_t state;
  char     buf[PROTO_MAX_FRAME];
  uint8_t  len;
  uint8_t  cs_calc;
  char     cs_txt[2];
  uint8_t  cs_len;
  /* statistics */
  uint32_t ok_count;
  uint32_t err_checksum;
  uint32_t err_format;
} proto_parser_t;

/** Encodes a frame. Returns the length written (excluding '\0'), 0 on error. */
size_t proto_encode(const proto_frame_t *f, char *out, size_t out_size);

/** Initialises the parser. */
void proto_parser_init(proto_parser_t *p);

/**
 * Feeds one received byte to the parser (callable from a task reading a
 * ring buffer filled by the UART ISR). Returns 1 when a complete, valid
 * frame has been decoded into *out, 0 otherwise.
 */
int proto_parser_feed(proto_parser_t *p, char c, proto_frame_t *out);

#ifdef __cplusplus
}
#endif

#endif /* PROTOCOL_H */
