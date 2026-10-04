/*
 * ============================================================================
 *  Machine Monitor — Nœud d'acquisition STM32 (Nucleo-C031C6, Cortex-M0+)
 *  FreeRTOS · I2C · ADC · GPIO · EXTI · UART — simulable dans Wokwi
 * ============================================================================
 *
 *   VibTask (100 Hz) ──┐                         ┌──────────────┐  USART2
 *   EnvTask (0,5 Hz) ──┼─► g_measure (mutex) ─►  │ AnalysisTask │─► frameQueue ─► CommTask ──► ESP32 / passerelle
 *                      │                         │    (1 Hz)    │                   ▲
 *   ISR bouton ────────┴─► notification ───────► └──────┬───────┘                   │ commandes "F1"/"F0"
 *                                                       ▼
 *                                     LEDs verte/jaune/rouge + BuzzerTask
 *
 *  Câblage (diagram.json) :
 *    DHT22 → PA1 | MPU6050 → I2C logiciel (PB8 SCL, PB9 SDA) | Potentiomètre (ACS712) → PA0
 *    LEDs PB13/PB14/PB15 | Buzzer PB10 | Bouton "Panne" PB11 | USART2 PA2 TX / PA3 RX
 *
 *  Optimisé pour 32 Ko de flash / 12 Ko de RAM :
 *   - aucun calcul flottant (le Cortex-M0+ n'a pas de FPU) ;
 *   - pilotes minimaux écrits à la main (DHT22, I2C, buzzer) au lieu de
 *     bibliothèques génériques ;
 *   - encodeur de trame sans printf (protocol.c).
 *
 *  Chaque seconde, une trame est envoyée sur USART2 (format dans protocol.h) :
 *    $MM,<seq>,<temp×10>,<hum×10>,<vib mg>,<crête mg>,<courant mA>,<flags>*<XOR>
 * ============================================================================
 */
#include <Arduino.h>
#include <STM32FreeRTOS.h>
#include "protocol.h"

// ---------------------------------------------------------------- Brochage
#define PIN_DHT        PA1
#define PIN_CURRENT    PA0
#define PIN_SCL        PB8
#define PIN_SDA        PB9
#define PIN_LED_OK     PB13
#define PIN_LED_WARN   PB14
#define PIN_LED_ALARM  PB15
#define PIN_BUZZER     PB10
#define PIN_BUTTON     PB11

// USART2 (PA2 TX / PA3 RX) = port série par défaut de la Nucleo (ST-Link)
#define Link Serial

// ---------------------------------------------------------------- Seuils
#define TEMP_WARN_D    600      // dixièmes de °C
#define TEMP_CRIT_D    750
#define VIB_WARN_MG    300      // milli-g RMS
#define VIB_CRIT_MG    600
#define CURR_WARN_MA   3500
#define CURR_CRIT_MA   4500

// ACS712-05B (185 mV/A sous 5 V) ramené en 3,3 V par pont diviseur :
// zéro à 1,65 V, sensibilité 122 mV/A
#define ACS_ZERO_MV    1650
#define ACS_SENS_UV_MA 122

#define VIB_PERIOD_MS  10       // 100 Hz
#define VIB_WINDOW     100      // fenêtre RMS = 1 s

// ---------------------------------------------------------------- Données partagées
typedef struct {
  int16_t  temp_d;
  uint16_t hum_d, vib_rms_mg, vib_peak_mg, curr_ma;
  uint8_t  dht_ok, mpu_ok;
} measure_t;

enum Level : uint8_t { LVL_NORMAL = 0, LVL_WARNING, LVL_CRITICAL };

static measure_t         g_measure = {};
static SemaphoreHandle_t measureMutex;    // protège g_measure
static QueueHandle_t     frameQueue;      // AnalysisTask → CommTask
static TaskHandle_t      analysisHandle;
static TaskHandle_t      buzzerHandle;
static volatile bool     g_fault = false; // panne simulée (bouton ou commande)
static volatile uint32_t g_lastIsrMs = 0;
static bool              g_mpuOk = false;

// ============================================================================
//  Racine carrée entière (méthode bit à bit) — pas de flottant sur M0+
// ============================================================================
static uint32_t isqrt(uint32_t x) {
  uint32_t r = 0, b = 1UL << 30;
  while (b > x) b >>= 2;
  while (b) {
    if (x >= r + b) { x -= r + b; r = (r >> 1) + b; }
    else r >>= 1;
    b >>= 2;
  }
  return r;
}

// ============================================================================
//  I2C logiciel ("bit-banging") — quelques centaines d'octets au lieu de
//  plusieurs Ko pour la bibliothèque Wire + HAL I2C.
//  Sortie "collecteur ouvert" émulée : niveau bas = sortie à 0,
//  niveau haut = broche relâchée (entrée avec pull-up).
// ============================================================================
#define I2C_DELAY() delayMicroseconds(3)

static inline void sclHigh() { pinMode(PIN_SCL, INPUT_PULLUP); }
static inline void sclLow()  { pinMode(PIN_SCL, OUTPUT); digitalWrite(PIN_SCL, LOW); }
static inline void sdaHigh() { pinMode(PIN_SDA, INPUT_PULLUP); }
static inline void sdaLow()  { pinMode(PIN_SDA, OUTPUT); digitalWrite(PIN_SDA, LOW); }

static void i2cStart() { sdaHigh(); sclHigh(); I2C_DELAY(); sdaLow(); I2C_DELAY(); sclLow(); }
static void i2cStop()  { sdaLow(); I2C_DELAY(); sclHigh(); I2C_DELAY(); sdaHigh(); I2C_DELAY(); }

// Envoie un octet ; renvoie true si l'esclave a acquitté (ACK)
static bool i2cWrite(uint8_t b) {
  for (int i = 0; i < 8; i++) {
    if (b & 0x80) sdaHigh(); else sdaLow();
    b <<= 1;
    I2C_DELAY(); sclHigh(); I2C_DELAY(); sclLow();
  }
  sdaHigh(); I2C_DELAY(); sclHigh(); I2C_DELAY();
  bool ack = digitalRead(PIN_SDA) == LOW;
  sclLow();
  return ack;
}

static uint8_t i2cRead(bool ack) {
  uint8_t b = 0;
  sdaHigh();
  for (int i = 0; i < 8; i++) {
    I2C_DELAY(); sclHigh(); I2C_DELAY();
    b = (uint8_t)((b << 1) | (digitalRead(PIN_SDA) == HIGH ? 1 : 0));
    sclLow();
  }
  if (ack) sdaLow(); else sdaHigh();
  I2C_DELAY(); sclHigh(); I2C_DELAY(); sclLow();
  sdaHigh();
  return b;
}

// ============================================================================
//  MPU6050 : accéléromètre (registres du datasheet)
// ============================================================================
#define MPU_ADDR 0x68

static bool mpuWriteReg(uint8_t reg, uint8_t val) {
  i2cStart();
  bool ok = i2cWrite(MPU_ADDR << 1) && i2cWrite(reg) && i2cWrite(val);
  i2cStop();
  return ok;
}

static bool mpuReadRegs(uint8_t reg, uint8_t *buf, uint8_t n) {
  i2cStart();
  bool ok = i2cWrite(MPU_ADDR << 1) && i2cWrite(reg);
  i2cStop();
  if (!ok) return false;
  i2cStart();
  if (!i2cWrite((MPU_ADDR << 1) | 1)) { i2cStop(); return false; }
  for (uint8_t i = 0; i < n; i++) buf[i] = i2cRead(i + 1 < n);   // NACK sur le dernier
  i2cStop();
  return true;
}

static bool mpuInit() {
  uint8_t id = 0;
  if (!mpuReadRegs(0x75, &id, 1) || id != 0x68) return false;   // WHO_AM_I
  return mpuWriteReg(0x6B, 0x00)      // PWR_MGMT_1 : réveil
      && mpuWriteReg(0x1A, 0x03)      // CONFIG : filtre passe-bas 44 Hz
      && mpuWriteReg(0x1C, 0x08);     // ACCEL_CONFIG : ±4 g → 8192 LSB/g
}

// Norme de l'accélération en milli-g (−1 si erreur)
static int32_t mpuReadMagnitudeMg() {
  uint8_t d[6];
  if (!mpuReadRegs(0x3B, d, 6)) return -1;                       // ACCEL_XOUT_H
  int16_t ax = (int16_t)((d[0] << 8) | d[1]);
  int16_t ay = (int16_t)((d[2] << 8) | d[3]);
  int16_t az = (int16_t)((d[4] << 8) | d[5]);
  uint32_t sq = (uint32_t)((int32_t)ax * ax) + (uint32_t)((int32_t)ay * ay) + (uint32_t)((int32_t)az * az);
  return (int32_t)(isqrt(sq) * 1000UL / 8192UL);
}

// ============================================================================
//  DHT22 : protocole 1 fil propriétaire, décodé sans timer.
//  On compte les tours de boucle de chaque impulsion : pour chaque bit, une
//  impulsion haute plus longue que la basse (≈70 µs vs 50 µs) vaut 1,
//  plus courte (≈26 µs) vaut 0. Résultat en entiers (dixièmes).
// ============================================================================
static uint16_t pulseCount(int level) {
  uint16_t n = 0;
  while (digitalRead(PIN_DHT) == level) {
    if (++n == 0xFFFF) return 0;        // délai dépassé
  }
  return n;
}

static bool dhtRead(int16_t *temp_d, uint16_t *hum_d) {
  uint16_t cycles[80];
  uint8_t data[5] = {0};

  // Signal de départ : ligne à 0 pendant ≥ 1 ms, puis relâchée
  pinMode(PIN_DHT, OUTPUT);
  digitalWrite(PIN_DHT, LOW);
  vTaskDelay(pdMS_TO_TICKS(2));
  pinMode(PIN_DHT, INPUT_PULLUP);
  delayMicroseconds(55);                 // le capteur répond 20–40 µs après

  // Section critique ≈ 5 ms : aucune préemption pendant la mesure des impulsions
  bool ok = true;
  taskENTER_CRITICAL();
  if (pulseCount(LOW) == 0 || pulseCount(HIGH) == 0) ok = false;   // réponse 80 µs / 80 µs
  for (int i = 0; ok && i < 80; i += 2) {
    cycles[i]     = pulseCount(LOW);
    cycles[i + 1] = pulseCount(HIGH);
    if (cycles[i] == 0 || cycles[i + 1] == 0) ok = false;
  }
  taskEXIT_CRITICAL();
  if (!ok) return false;

  for (int i = 0; i < 40; i++) {
    data[i / 8] <<= 1;
    if (cycles[2 * i + 1] > cycles[2 * i]) data[i / 8] |= 1;
  }
  if ((uint8_t)(data[0] + data[1] + data[2] + data[3]) != data[4]) return false;

  *hum_d = (uint16_t)((data[0] << 8) | data[1]);
  int16_t t = (int16_t)(((data[2] & 0x7F) << 8) | data[3]);
  *temp_d = (data[2] & 0x80) ? (int16_t)-t : t;
  return true;
}

// ============================================================================
//  VibTask : échantillonnage 100 Hz, RMS de la composante alternative sur 1 s
// ============================================================================
static void VibTask(void *) {
  static int16_t win[VIB_WINDOW];
  uint16_t idx = 0;
  uint32_t n = 0;
  bool ok = g_mpuOk;
  Link.println("# tache vib demarree");
  TickType_t last = xTaskGetTickCount();

  for (;;) {
    vTaskDelayUntil(&last, pdMS_TO_TICKS(VIB_PERIOD_MS));   // période stricte

    int32_t s = ok ? mpuReadMagnitudeMg() : 1000;
    if (s < 0) { ok = false; s = 1000; }
    if (g_fault) {
      // Balourd simulé à 25 Hz : échantillonné à 100 Hz → 4 points par période
      static const int16_t SINE4[4] = { 0, 900, 0, -900 };
      s += SINE4[n & 3];
    }
    win[idx++] = (int16_t)constrain(s, -32000, 32000);
    n++;

    if (idx >= VIB_WINDOW) {
      idx = 0;
      int32_t mean = 0;
      for (int i = 0; i < VIB_WINDOW; i++) mean += win[i];
      mean /= VIB_WINDOW;                            // retire gravité/inclinaison
      uint32_t sumsq = 0;
      int32_t peak = 0;
      for (int i = 0; i < VIB_WINDOW; i++) {
        int32_t d = win[i] - mean;
        sumsq += (uint32_t)(d * d);
        if (d < 0) d = -d;
        if (d > peak) peak = d;
      }
      xSemaphoreTake(measureMutex, portMAX_DELAY);
      g_measure.vib_rms_mg  = (uint16_t)isqrt(sumsq / VIB_WINDOW);
      g_measure.vib_peak_mg = (uint16_t)peak;
      g_measure.mpu_ok      = ok;
      xSemaphoreGive(measureMutex);
    }
  }
}

// ============================================================================
//  EnvTask : DHT22 (toutes les 2 s) + courant ACS712 (ADC, moyenne de 32)
// ============================================================================
static void EnvTask(void *) {
  Link.println("# tache env demarree");
  for (;;) {
    uint32_t sum = 0;
    for (int i = 0; i < 32; i++) sum += analogRead(PIN_CURRENT);
    int32_t mv = (int32_t)((sum / 32) * 3300UL / 4095UL);
    int32_t ma = (mv - ACS_ZERO_MV) * 1000L / ACS_SENS_UV_MA;
    if (ma < 0) ma = -ma;
    if (g_fault) ma += 2500;                         // surintensité simulée
    if (ma > 65535) ma = 65535;

    int16_t t = 0;
    uint16_t h = 0;
    bool dhtOk = dhtRead(&t, &h);

    xSemaphoreTake(measureMutex, portMAX_DELAY);
    g_measure.curr_ma = (uint16_t)ma;
    g_measure.dht_ok  = dhtOk;
    if (dhtOk) {
      g_measure.temp_d = (int16_t)(t + (g_fault ? 350 : 0));
      g_measure.hum_d  = h;
    }
    xSemaphoreGive(measureMutex);

    vTaskDelay(pdMS_TO_TICKS(2000));
  }
}

// ============================================================================
//  BuzzerTask : bip de 300 ms à ~500 Hz quand on la notifie
//  (signal carré généré par la tâche : pas besoin de timer matériel)
// ============================================================================
static void BuzzerTask(void *) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    for (int i = 0; i < 300; i++) {
      digitalWrite(PIN_BUZZER, (i & 1) ? HIGH : LOW);
      vTaskDelay(1);                                 // 1 ms → période 2 ms
    }
    digitalWrite(PIN_BUZZER, LOW);
  }
}

// ============================================================================
//  AnalysisTask : niveaux d'alerte, LEDs/buzzer, construction de la trame
// ============================================================================
static Level levelOf(int32_t v, int32_t warn, int32_t crit) {
  return v >= crit ? LVL_CRITICAL : (v >= warn ? LVL_WARNING : LVL_NORMAL);
}

static void AnalysisTask(void *) {
  uint16_t seq = 0;
  Link.println("# tache analyse demarree");
  TickType_t last = xTaskGetTickCount();

  for (;;) {
    vTaskDelayUntil(&last, pdMS_TO_TICKS(1000));

    // Appui bouton signalé par l'ISR ? (lecture non bloquante)
    if (ulTaskNotifyTake(pdTRUE, 0) > 0) g_fault = !g_fault;

    measure_t m;
    xSemaphoreTake(measureMutex, portMAX_DELAY);
    m = g_measure;
    xSemaphoreGive(measureMutex);

    Level lt = levelOf(m.temp_d, TEMP_WARN_D, TEMP_CRIT_D);
    Level lv = levelOf(m.vib_rms_mg, VIB_WARN_MG, VIB_CRIT_MG);
    Level lc = levelOf(m.curr_ma, CURR_WARN_MA, CURR_CRIT_MA);
    Level g = lt;
    if (lv > g) g = lv;
    if (lc > g) g = lc;

    digitalWrite(PIN_LED_OK,    g == LVL_NORMAL   ? HIGH : LOW);
    digitalWrite(PIN_LED_WARN,  g == LVL_WARNING  ? HIGH : LOW);
    digitalWrite(PIN_LED_ALARM, g == LVL_CRITICAL ? HIGH : LOW);
    if (g == LVL_CRITICAL) xTaskNotifyGive(buzzerHandle);

    proto_frame_t f;
    f.seq     = seq++;
    f.temp_d  = m.temp_d;
    f.hum_d   = m.hum_d;
    f.vib_mg  = m.vib_rms_mg;
    f.peak_mg = m.vib_peak_mg;
    f.curr_ma = m.curr_ma;
    f.flags   = (uint8_t)((m.dht_ok ? PROTO_FLAG_DHT_OK : 0) |
                          (m.mpu_ok ? PROTO_FLAG_MPU_OK : 0) |
                          (g_fault  ? PROTO_FLAG_FAULT  : 0) |
                          ((uint8_t)g << PROTO_LEVEL_SHIFT));

    // File pleine → on remplace la plus ancienne (la donnée fraîche prime)
    if (xQueueSend(frameQueue, &f, 0) != pdTRUE) {
      proto_frame_t old;
      xQueueReceive(frameQueue, &old, 0);
      xQueueSend(frameQueue, &f, 0);
    }
  }
}

// ============================================================================
//  CommTask : envoi des trames sur USART2 + réception des commandes
//    "F1" = panne ON, "F0" = panne OFF (envoyées par l'ESP32 / la passerelle)
// ============================================================================
static void CommTask(void *) {
  char buf[PROTO_MAX_FRAME];
  char prev = 0;
  proto_frame_t f;
  Link.println("# tache comm demarree");
  for (;;) {
    if (xQueueReceive(frameQueue, &f, pdMS_TO_TICKS(20)) == pdTRUE) {
      size_t n = proto_encode(&f, buf, sizeof buf);
      if (n) Link.write((const uint8_t *)buf, n);
    }
    while (Link.available()) {
      char c = (char)Link.read();
      if (prev == 'F' && c == '1') { g_fault = true;  Link.println("# panne ON"); }
      if (prev == 'F' && c == '0') { g_fault = false; Link.println("# panne OFF"); }
      prev = c;
    }
  }
}

// ============================================================================
//  ISR du bouton : courte, elle ne fait que réveiller la tâche d'analyse
// ============================================================================
static void onButton() {
  uint32_t now = millis();
  if (now - g_lastIsrMs < 200) return;               // anti-rebond
  g_lastIsrMs = now;
  BaseType_t woken = pdFALSE;
  vTaskNotifyGiveFromISR(analysisHandle, &woken);
  portYIELD_FROM_ISR(woken);
}

// ============================================================================
void setup() {
  Link.begin(115200);
  Link.println("# Machine Monitor - STM32C031 / FreeRTOS");

  pinMode(PIN_LED_OK, OUTPUT);
  pinMode(PIN_LED_WARN, OUTPUT);
  pinMode(PIN_LED_ALARM, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  digitalWrite(PIN_LED_OK, HIGH);                    // signe de vie immédiat
  analogReadResolution(12);

  // Capteurs initialisés AVANT FreeRTOS (plus simple à diagnostiquer)
  sdaHigh();
  sclHigh();
  g_mpuOk = mpuInit();
  Link.println(g_mpuOk ? "# MPU6050 OK" : "# MPU6050 absent (vibration desactivee)");
  pinMode(PIN_DHT, INPUT_PULLUP);
  Link.println("# DHT22 pret");

  measureMutex = xSemaphoreCreateMutex();            // mutex avec héritage de priorité
  frameQueue   = xQueueCreate(4, sizeof(proto_frame_t));

  // Piles en mots de 4 octets (12 Ko de RAM au total sur ce microcontrôleur)
  BaseType_t ok = pdTRUE;
  ok &= xTaskCreate(VibTask,      "vib",      160, NULL, tskIDLE_PRIORITY + 4, NULL);
  ok &= xTaskCreate(EnvTask,      "env",      200, NULL, tskIDLE_PRIORITY + 3, NULL);
  ok &= xTaskCreate(AnalysisTask, "analysis", 160, NULL, tskIDLE_PRIORITY + 3, &analysisHandle);
  ok &= xTaskCreate(CommTask,     "comm",     160, NULL, tskIDLE_PRIORITY + 2, NULL);
  ok &= xTaskCreate(BuzzerTask,   "buzzer",    96, NULL, tskIDLE_PRIORITY + 1, &buzzerHandle);
  Link.println(ok == pdTRUE ? "# taches creees, demarrage FreeRTOS" : "# ERREUR creation des taches (memoire)");

  attachInterrupt(digitalPinToInterrupt(PIN_BUTTON), onButton, FALLING);

  vTaskStartScheduler();                             // ne revient jamais
  Link.println("# ERREUR : memoire insuffisante pour FreeRTOS");
}

void loop() {
  // Jamais exécuté : le planificateur FreeRTOS a pris la main.
}
