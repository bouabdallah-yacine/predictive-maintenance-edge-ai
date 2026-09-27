/*
 * ============================================================================
 *  Machine Monitor — Nœud d'acquisition STM32 (Nucleo-C031C6, Cortex-M0+)
 *  FreeRTOS · I2C · ADC · GPIO · EXTI · UART — simulable dans Wokwi
 * ============================================================================
 *
 *   VibTask (100 Hz) ──┐                         ┌──────────────┐  USART1
 *   EnvTask (0,5 Hz) ──┼─► g_measure (mutex) ─►  │ AnalysisTask │─► frameQueue ─► CommTask ──► ESP32 / passerelle
 *                      │                         │    (1 Hz)    │                   ▲
 *   ISR bouton ────────┴─► notification ───────► └──────┬───────┘                   │ commandes "F1"/"F0"
 *                                                       ▼
 *                                           LEDs verte/jaune/rouge + buzzer
 *
 *  Câblage (diagram.json) :
 *    DHT22 → PA1 | MPU6050 → I2C1 (PB8 SCL, PB9 SDA) | Potentiomètre (ACS712) → PA0
 *    LEDs PB13/PB14/PB15 | Buzzer PB10 | Bouton "Panne" PB11 | USART2 PA2 TX / PA3 RX
 *
 *  Le Cortex-M0+ n'a pas d'unité flottante : les calculs de vibration sont
 *  faits en entiers (racine carrée entière, motif sinusoïdal précalculé).
 *
 *  Chaque seconde, une trame est envoyée sur USART1 (format dans protocol.h) :
 *    $MM,<seq>,<temp×10>,<hum×10>,<vib mg>,<crête mg>,<courant mA>,<flags>*<XOR>
 * ============================================================================
 */
#include <Arduino.h>
#include <Wire.h>
#include <STM32FreeRTOS.h>
#include <DHT.h>
#include "protocol.h"

// ---------------------------------------------------------------- Brochage
#define PIN_DHT        PA1
#define PIN_CURRENT    PA0
#define PIN_LED_OK     PB13
#define PIN_LED_WARN   PB14
#define PIN_LED_ALARM  PB15
#define PIN_BUZZER     PB10
#define PIN_BUTTON     PB11

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

// 1 = MPU6050 utilisé ; 0 = désactivé (la vibration ne vient alors que du
// bouton "Panne"). Utile si le bus I2C pose problème.
#ifndef USE_MPU6050
#define USE_MPU6050    1
#endif

#define VIB_PERIOD_MS  10       // 100 Hz
#define VIB_WINDOW     100      // fenêtre RMS = 1 s

// ---------------------------------------------------------------- Objets
// USART2 (PA2 TX / PA3 RX) = port série par défaut de la Nucleo (ST-Link)
#define Link Serial
DHT dht(PIN_DHT, DHT22);

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
static volatile bool     g_fault = false; // panne simulée (bouton ou commande)
static volatile uint32_t g_lastIsrMs = 0;
static bool             g_mpuOk = false;   // résultat de l'init MPU6050 (faite dans setup)

// ============================================================================
//  MPU6050 : pilote I2C minimal (registres du datasheet)
// ============================================================================
#define MPU_ADDR 0x68

// Racine carrée entière (méthode bit à bit) : pas de flottant sur Cortex-M0+
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

static bool mpuWrite(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

// Lecture de registres : écriture de l'adresse (avec STOP), puis lecture.
// (Pas de "repeated start" : plus simple et mieux supporté par le simulateur.)
static bool mpuInit() {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x75);                                  // WHO_AM_I
  if (Wire.endTransmission() != 0) return false;
  if (Wire.requestFrom(MPU_ADDR, 1) != 1 || Wire.read() != 0x68) return false;
  return mpuWrite(0x6B, 0x00)      // PWR_MGMT_1 : réveil
      && mpuWrite(0x1A, 0x03)      // CONFIG : filtre passe-bas 44 Hz
      && mpuWrite(0x1C, 0x08);     // ACCEL_CONFIG : ±4 g → 8192 LSB/g
}

// Norme de l'accélération en milli-g (−1 si erreur)
static int32_t mpuReadMagnitudeMg() {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x3B);                                  // ACCEL_XOUT_H
  if (Wire.endTransmission() != 0) return -1;
  if (Wire.requestFrom(MPU_ADDR, 6) != 6) return -1;
  uint8_t d[6];
  for (int i = 0; i < 6; i++) d[i] = Wire.read();    // ordre de lecture garanti
  int16_t ax = (int16_t)((d[0] << 8) | d[1]);
  int16_t ay = (int16_t)((d[2] << 8) | d[3]);
  int16_t az = (int16_t)((d[4] << 8) | d[5]);
  // |a| en LSB (±4 g → 8192 LSB/g), puis conversion en milli-g
  uint32_t sq = (uint32_t)((int32_t)ax * ax) + (uint32_t)((int32_t)ay * ay) + (uint32_t)((int32_t)az * az);
  return (int32_t)(isqrt(sq) * 1000UL / 8192UL);
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

    int32_t mag = ok ? mpuReadMagnitudeMg() : -1;
    if (mag < 0) { ok = false; mag = 1000; }
    int32_t s = mag;
    if (g_fault) {                                   // balourd simulé à 25 Hz
      // 25 Hz échantillonné à 100 Hz = 4 points par période : sin = 0, 1, 0, -1
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
        if (abs(d) > peak) peak = abs(d);
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
    int32_t ma = labs((mv - ACS_ZERO_MV) * 1000L / ACS_SENS_UV_MA);
    if (g_fault) ma += 2500;                         // surintensité simulée

    float t = dht.readTemperature();
    float h = dht.readHumidity();
    bool dhtOk = !isnan(t) && !isnan(h);

    xSemaphoreTake(measureMutex, portMAX_DELAY);
    g_measure.curr_ma = (uint16_t)(ma > 65535 ? 65535 : ma);
    g_measure.dht_ok  = dhtOk;
    if (dhtOk) {
      g_measure.temp_d = (int16_t)(t * 10.0f) + (g_fault ? 350 : 0);
      g_measure.hum_d  = (uint16_t)(h * 10.0f);
    }
    xSemaphoreGive(measureMutex);

    vTaskDelay(pdMS_TO_TICKS(2000));
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
    if (g == LVL_CRITICAL) tone(PIN_BUZZER, 2000, 300);

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
//  CommTask : envoi des trames sur USART1 + réception des commandes
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

  // Initialisation des capteurs AVANT FreeRTOS (plus simple à diagnostiquer)
#if USE_MPU6050
  Link.println("# init I2C...");
  Wire.setSDA(PB9);
  Wire.setSCL(PB8);
  Wire.begin();
  Link.println("# I2C pret, detection MPU6050...");
  g_mpuOk = mpuInit();
  Link.println(g_mpuOk ? "# MPU6050 OK" : "# MPU6050 absent (vibration desactivee)");
#else
  Link.println("# MPU6050 desactive (USE_MPU6050 = 0)");
#endif
  dht.begin();
  Link.println("# DHT22 pret");

  measureMutex = xSemaphoreCreateMutex();            // mutex avec héritage de priorité
  frameQueue   = xQueueCreate(8, sizeof(proto_frame_t));

  //          fonction      nom         pile(mots) param prio               handle
  BaseType_t ok = pdTRUE;
  // Piles en mots de 4 octets (12 Ko de RAM au total sur ce microcontrôleur)
  ok &= xTaskCreate(VibTask,      "vib",      160, NULL, tskIDLE_PRIORITY + 4, NULL);
  ok &= xTaskCreate(EnvTask,      "env",      256, NULL, tskIDLE_PRIORITY + 3, NULL);
  ok &= xTaskCreate(AnalysisTask, "analysis", 160, NULL, tskIDLE_PRIORITY + 3, &analysisHandle);
  ok &= xTaskCreate(CommTask,     "comm",     200, NULL, tskIDLE_PRIORITY + 2, NULL);
  Link.println(ok == pdTRUE ? "# taches creees, demarrage FreeRTOS" : "# ERREUR creation des taches (memoire)");

  attachInterrupt(digitalPinToInterrupt(PIN_BUTTON), onButton, FALLING);

  vTaskStartScheduler();                             // ne revient jamais
  Link.println("# ERREUR : mémoire insuffisante pour FreeRTOS");
}

void loop() {
  // Jamais exécuté : le planificateur FreeRTOS a pris la main.
}
