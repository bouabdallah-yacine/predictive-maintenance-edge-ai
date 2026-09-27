/**
 * @file    protocol.h
 * @brief   Trame UART STM32 → ESP32 (format texte type NMEA, avec checksum).
 *
 *  $MM,<seq>,<temp_d>,<hum_d>,<vib_mg>,<peak_mg>,<curr_ma>,<flags>*<CS>\r\n
 *
 *   seq      : compteur 0..65535 (détecte les trames perdues)
 *   temp_d   : température en dixièmes de °C (ex. 253 = 25,3 °C), signé
 *   hum_d    : humidité en dixièmes de %
 *   vib_mg   : vibration RMS en milli-g
 *   peak_mg  : vibration crête en milli-g
 *   curr_ma  : courant en mA
 *   flags    : bit0 DHT ok | bit1 MPU ok | bit2 panne simulée
 *              bits 4-5 niveau global (0 normal, 1 warning, 2 critique)
 *   CS       : XOR de tous les octets entre '$' et '*', en hexadécimal (2 car.)
 *
 *  Pourquoi des entiers ? printf("%f") est désactivé par défaut avec
 *  newlib-nano (économie de ~10 Ko de flash) et les entiers se décodent sans
 *  ambiguïté. Exemple :
 *    $MM,42,253,451,85,140,1620,3*20\r\n
 *
 *  Ce module est du C pur (aucune dépendance HAL) → testé sur PC (test/).
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
  /* statistiques */
  uint32_t ok_count;
  uint32_t err_checksum;
  uint32_t err_format;
} proto_parser_t;

/** Encode une trame. Retourne la longueur écrite (sans '\0'), 0 si erreur. */
size_t proto_encode(const proto_frame_t *f, char *out, size_t out_size);

/** Initialise le parseur. */
void proto_parser_init(proto_parser_t *p);

/**
 * Fournit un octet reçu au parseur (appelable depuis une tâche qui lit un
 * ring buffer rempli par l'ISR UART). Retourne 1 quand une trame valide
 * complète a été décodée dans *out, 0 sinon.
 */
int proto_parser_feed(proto_parser_t *p, char c, proto_frame_t *out);

#ifdef __cplusplus
}
#endif

#endif /* PROTOCOL_H */
