/**
 * @file    protocol.c
 * @brief   Encoding / decoding of UART frames (see protocol.h).
 */
#include "protocol.h"
#include <stdlib.h>
#include <string.h>

static const char HEX[] = "0123456789ABCDEF";

/* Writes an integer in decimal (no printf: lightweight and deterministic). */
static char *put_int(char *p, const char *end, int32_t v)
{
  char tmp[12];
  int n = 0;
  uint32_t u = (v < 0) ? (uint32_t)(-(int64_t)v) : (uint32_t)v;
  do { tmp[n++] = (char)('0' + (u % 10u)); u /= 10u; } while (u && n < (int)sizeof tmp);
  if (v < 0) tmp[n++] = '-';
  if (p + n > end) return NULL;
  while (n) *p++ = tmp[--n];
  return p;
}

size_t proto_encode(const proto_frame_t *f, char *out, size_t out_size)
{
  if (!f || !out || out_size < 16) return 0;

  const char *end = out + out_size - 5;          /* room for CS, \r\n, \0 */
  const int32_t fields[7] = { f->seq, f->temp_d, f->hum_d, f->vib_mg,
                              f->peak_mg, f->curr_ma, f->flags };
  char *p = out;
  *p++ = '$'; *p++ = 'M'; *p++ = 'M';
  for (int i = 0; i < 7; i++) {
    if (p >= end) return 0;
    *p++ = ',';
    p = put_int(p, end, fields[i]);
    if (!p) return 0;
  }
  if (p >= end) return 0;

  uint8_t cs = 0;                                 /* XOR between '$' and '*' */
  for (char *q = out + 1; q < p; q++) cs ^= (uint8_t)*q;

  *p++ = '*';
  *p++ = HEX[cs >> 4];
  *p++ = HEX[cs & 0x0F];
  *p++ = '\r';
  *p++ = '\n';
  *p   = '\0';
  return (size_t)(p - out);
}

void proto_parser_init(proto_parser_t *p)
{
  memset(p, 0, sizeof(*p));
  p->state = PROTO_WAIT_START;
}

static int hexval(char c)
{
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

/* Splits "MM,a,b,c,d,e,f,g" into fields and checks the bounds. */
static int parse_body(const char *body, proto_frame_t *out)
{
  if (strncmp(body, "MM,", 3) != 0) return 0;
  const char *s = body + 3;
  long v[7];
  for (int i = 0; i < 7; i++) {
    char *end;
    v[i] = strtol(s, &end, 10);
    if (end == s) return 0;                          /* empty field */
    if (i < 6 && *end != ',') return 0;
    if (i == 6 && *end != '\0') return 0;
    s = end + 1;
  }
  if (v[0] < 0 || v[0] > 65535) return 0;
  if (v[1] < -400 || v[1] > 1250) return 0;          /* -40..125 °C */
  for (int i = 2; i < 6; i++) if (v[i] < 0 || v[i] > 65535) return 0;
  if (v[6] < 0 || v[6] > 255) return 0;

  out->seq     = (uint16_t)v[0];
  out->temp_d  = (int16_t)v[1];
  out->hum_d   = (uint16_t)v[2];
  out->vib_mg  = (uint16_t)v[3];
  out->peak_mg = (uint16_t)v[4];
  out->curr_ma = (uint16_t)v[5];
  out->flags   = (uint8_t)v[6];
  return 1;
}

int proto_parser_feed(proto_parser_t *p, char c, proto_frame_t *out)
{
  /* A '$' always restarts synchronisation (resync after noise) */
  if (c == '$') {
    p->state = PROTO_IN_BODY;
    p->len = 0;
    p->cs_calc = 0;
    p->cs_len = 0;
    return 0;
  }

  switch (p->state) {
  case PROTO_WAIT_START:
    return 0;

  case PROTO_IN_BODY:
    if (c == '*') {
      p->buf[p->len] = '\0';
      p->state = PROTO_IN_CS;
      return 0;
    }
    if (c == '\r' || c == '\n' || p->len >= PROTO_MAX_FRAME - 1) {
      p->err_format++;
      p->state = PROTO_WAIT_START;
      return 0;
    }
    p->buf[p->len++] = c;
    p->cs_calc ^= (uint8_t)c;
    return 0;

  case PROTO_IN_CS: {
    p->cs_txt[p->cs_len++] = c;
    if (p->cs_len < 2) return 0;
    p->state = PROTO_WAIT_START;
    int hi = hexval(p->cs_txt[0]), lo = hexval(p->cs_txt[1]);
    if (hi < 0 || lo < 0) { p->err_format++; return 0; }
    if ((uint8_t)((hi << 4) | lo) != p->cs_calc) { p->err_checksum++; return 0; }
    if (!parse_body(p->buf, out)) { p->err_format++; return 0; }
    p->ok_count++;
    return 1;
  }
  }
  return 0;
}
