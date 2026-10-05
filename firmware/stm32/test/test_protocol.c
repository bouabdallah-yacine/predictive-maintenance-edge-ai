/* Protocol unit tests — compiled and run on a PC:
 *   gcc -Wall -Wextra -I../Core/Inc test_protocol.c ../Core/Src/protocol.c -o t && ./t
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "protocol.h"

static int feed_str(proto_parser_t *p, const char *s, proto_frame_t *out)
{
  int got = 0;
  for (; *s; s++) got += proto_parser_feed(p, *s, out);
  return got;
}

int main(int argc, char **argv)
{
  char buf[PROTO_MAX_FRAME];
  proto_parser_t p;
  proto_frame_t in = { 42, 253, 451, 85, 140, 1620, 3 }, out;

  /* 1. Encode → decode round trip */
  size_t n = proto_encode(&in, buf, sizeof buf);
  assert(n > 0 && buf[n - 2] == '\r' && buf[n - 1] == '\n');
  printf("frame: %s", buf);
  proto_parser_init(&p);
  assert(feed_str(&p, buf, &out) == 1);
  assert(out.seq == in.seq && out.temp_d == in.temp_d && out.hum_d == in.hum_d &&
         out.vib_mg == in.vib_mg && out.peak_mg == in.peak_mg &&
         out.curr_ma == in.curr_ma && out.flags == in.flags);

  /* 2. Negative temperature */
  proto_frame_t neg = { 1, -125, 300, 0, 0, 0, 0 };
  proto_encode(&neg, buf, sizeof buf);
  assert(feed_str(&p, buf, &out) == 1 && out.temp_d == -125);

  /* 3. Corrupted checksum → rejected */
  proto_encode(&in, buf, sizeof buf);
  char bad[PROTO_MAX_FRAME]; strcpy(bad, buf); bad[5] = '9';
  assert(feed_str(&p, bad, &out) == 0 && p.err_checksum == 1);

  /* 4. Noise + truncated frame then a valid frame → resynchronisation */
  char noisy[256];
  snprintf(noisy, sizeof noisy, "xx\x01$MM,1,2$garbage%s", buf);
  assert(feed_str(&p, noisy, &out) == 1 && out.seq == 42);

  /* 5. Missing field (valid checksum but wrong format) → rejected */
  const char *body = "MM,1,2,3";
  unsigned char cs = 0; for (const char *c = body; *c; c++) cs ^= (unsigned char)*c;
  char f5[64]; snprintf(f5, sizeof f5, "$%s*%02X\r\n", body, cs);
  unsigned before = p.err_format;
  assert(feed_str(&p, f5, &out) == 0 && p.err_format == before + 1);

  /* 6. Output buffer too small */
  assert(proto_encode(&in, buf, 10) == 0);

  /* "dump" mode: generates frames to test the Python gateway */
  if (argc > 1 && strcmp(argv[1], "--dump") == 0) {
    for (uint16_t i = 0; i < 3; i++) {
      proto_frame_t f = { i, (int16_t)(250 + i * 100), 450, (uint16_t)(50 + i * 300), 120, (uint16_t)(1500 + i * 1500), 3 };
      proto_encode(&f, buf, sizeof buf);
      fputs(buf, stderr);
    }
  }
  printf("OK: all tests pass (%u valid frames, %u checksum errors, %u format errors)\n",
         (unsigned)p.ok_count, (unsigned)p.err_checksum, (unsigned)p.err_format);
  return 0;
}
