/*
 * ============================================================================
 *  Machine Monitor — STM32 acquisition node (Nucleo-C031C6, Cortex-M0+)
 *  FreeRTOS · I2C · ADC · GPIO · EXTI · UART — can be simulated in Wokwi
 * ============================================================================
 *
 *   VibTask (100 Hz) ──┐                         ┌──────────────┐  USART2
 *   EnvTask (0.5 Hz) ──┼─► g_measure (mutex) ─►  │ AnalysisTask │─► frameQueue ─► CommTask ──► ESP32 / gateway
 *                      │                         │    (1 Hz)    │                   ▲
 *   button ISR ────────┴─► notification ───────► └──────┬───────┘                   │ "F1"/"F0" commands
 *                                                       ▼
 *                                     green/yellow/red LEDs + BuzzerTask
 *
 *  Wiring (diagram.json):
 *    NTC (motor temperature) → PA1 | HIH-4030 humidity (potentiometer) → PA4
 *    MPU6050 → software I2C (PB8 SCL, PB9 SDA) | Potentiometer (ACS712) → PA0
 *    LEDs PB13/PB14/PB15 | Buzzer PB10 | "Fault" button PB11 | USART2 PA2 TX / PA3 RX
 *
 *  Optimised for 32 KB of flash / 12 KB of RAM:
 *   - no floating-point maths (the Cortex-M0+ has no FPU);
 *   - minimal hand-written drivers (I2C, buzzer) instead of
 *     generic libraries;
 *   - NTC temperature converted with a precomputed table (no log/exp);
 *   - frame encoder without printf (protocol.c).
 *
 *  Every second, a frame is sent on USART2 (format in protocol.h):
 *    $MM,<seq>,<temp×10>,<hum×10>,<vib mg>,<peak mg>,<current mA>,<flags>*<XOR>
 * ============================================================================
 */
#include <Arduino.h>
#include <STM32FreeRTOS.h>
#include "stm32yyxx_ll_gpio.h"   // direct access to the GPIO registers (ST "LL" drivers)
#include "protocol.h"

// ---------------------------------------------------------------- Pinout
#define PIN_TEMP       PA1      // NTC thermistor (analog input)
#define PIN_HUM        PA4      // HIH-4030 analog humidity sensor (potentiometer in simulation)
#define PIN_CURRENT    PA0
#define PIN_SCL        PB8
#define PIN_SDA        PB9
#define PIN_LED_OK     PB13
#define PIN_LED_WARN   PB14
#define PIN_LED_ALARM  PB15
#define PIN_BUZZER     PB10
#define PIN_BUTTON     PB11

// USART2 (PA2 TX / PA3 RX) = default serial port of the Nucleo (ST-Link)
#define Link Serial

// ---------------------------------------------------------------- Thresholds
#define TEMP_WARN_D    600      // tenths of °C
#define TEMP_CRIT_D    750
#define VIB_WARN_MG    300      // milli-g RMS
#define VIB_CRIT_MG    600
#define CURR_WARN_MA   3500
#define CURR_CRIT_MA   4500

// ACS712-05B (185 mV/A at 5 V) scaled down to 3.3 V by a voltage divider:
// zero at 1.65 V, sensitivity 122 mV/A
#define ACS_ZERO_MV    1650
#define ACS_SENS_UV_MA 122

#define VIB_PERIOD_MS  10       // 100 Hz
#define VIB_WINDOW     100      // RMS window = 1 s

// ---------------------------------------------------------------- Shared data
typedef struct {
  int16_t  temp_d;
  uint16_t hum_d, vib_rms_mg, vib_peak_mg, curr_ma;
  uint8_t  dht_ok, mpu_ok;
} measure_t;

enum Level : uint8_t { LVL_NORMAL = 0, LVL_WARNING, LVL_CRITICAL };

static measure_t         g_measure = {};
static SemaphoreHandle_t measureMutex;    // protects g_measure
static QueueHandle_t     frameQueue;      // AnalysisTask → CommTask
static TaskHandle_t      analysisHandle;
static TaskHandle_t      buzzerHandle;
static volatile bool     g_fault = false; // simulated fault (button or command)
static volatile uint32_t g_lastIsrMs = 0;
static bool              g_mpuOk = false;

// ============================================================================
//  Integer square root (bit-by-bit method) — no floating point on M0+
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
//  Software I2C ("bit-banging") — a few hundred bytes instead of
//  several KB for the Wire library + HAL I2C.
//  Emulated "open-collector" output: low level = output driven to 0,
//  high level = pin released (input with pull-up).
// ============================================================================
#define I2C_DELAY() delayMicroseconds(2)

// PB8 = SCL, PB9 = SDA. The output register (ODR) is set to 0 once and for
// all and the pull-up resistor is enabled: from then on it is enough to
// switch the pin mode (MODER register) between "input" (= high level through
// the pull-up) and "output" (= low level). Only a few clock cycles, versus
// several microseconds for pinMode().
#define I2C_PORT    GPIOB
#define I2C_SCL_LL  LL_GPIO_PIN_8
#define I2C_SDA_LL  LL_GPIO_PIN_9

static inline void sclHigh() { LL_GPIO_SetPinMode(I2C_PORT, I2C_SCL_LL, LL_GPIO_MODE_INPUT); }
static inline void sclLow()  { LL_GPIO_SetPinMode(I2C_PORT, I2C_SCL_LL, LL_GPIO_MODE_OUTPUT); }
static inline void sdaHigh() { LL_GPIO_SetPinMode(I2C_PORT, I2C_SDA_LL, LL_GPIO_MODE_INPUT); }
static inline void sdaLow()  { LL_GPIO_SetPinMode(I2C_PORT, I2C_SDA_LL, LL_GPIO_MODE_OUTPUT); }
static inline bool sdaRead() { return LL_GPIO_IsInputPinSet(I2C_PORT, I2C_SDA_LL); }

static void i2cBegin() {
  pinMode(PIN_SCL, INPUT_PULLUP);                    // enables the port clock + pull-up
  pinMode(PIN_SDA, INPUT_PULLUP);
  LL_GPIO_SetPinOutputType(I2C_PORT, I2C_SCL_LL | I2C_SDA_LL, LL_GPIO_OUTPUT_PUSHPULL);
  LL_GPIO_ResetOutputPin(I2C_PORT, I2C_SCL_LL | I2C_SDA_LL);  // ODR = 0
}

static void i2cStart() { sdaHigh(); sclHigh(); I2C_DELAY(); sdaLow(); I2C_DELAY(); sclLow(); }
static void i2cStop()  { sdaLow(); I2C_DELAY(); sclHigh(); I2C_DELAY(); sdaHigh(); I2C_DELAY(); }

// Sends one byte; returns true if the slave acknowledged (ACK)
static bool i2cWrite(uint8_t b) {
  for (int i = 0; i < 8; i++) {
    if (b & 0x80) sdaHigh(); else sdaLow();
    b <<= 1;
    I2C_DELAY(); sclHigh(); I2C_DELAY(); sclLow();
  }
  sdaHigh(); I2C_DELAY(); sclHigh(); I2C_DELAY();
  bool ack = !sdaRead();
  sclLow();
  return ack;
}

static uint8_t i2cRead(bool ack) {
  uint8_t b = 0;
  sdaHigh();
  for (int i = 0; i < 8; i++) {
    I2C_DELAY(); sclHigh(); I2C_DELAY();
    b = (uint8_t)((b << 1) | (sdaRead() ? 1 : 0));
    sclLow();
  }
  if (ack) sdaLow(); else sdaHigh();
  I2C_DELAY(); sclHigh(); I2C_DELAY(); sclLow();
  sdaHigh();
  return b;
}

// ============================================================================
//  MPU6050: accelerometer (registers from the datasheet)
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
  for (uint8_t i = 0; i < n; i++) buf[i] = i2cRead(i + 1 < n);   // NACK on the last one
  i2cStop();
  return true;
}

static bool mpuInit() {
  uint8_t id = 0;
  if (!mpuReadRegs(0x75, &id, 1) || id != 0x68) return false;   // WHO_AM_I
  return mpuWriteReg(0x6B, 0x00)      // PWR_MGMT_1: wake up
      && mpuWriteReg(0x1A, 0x03)      // CONFIG: 44 Hz low-pass filter
      && mpuWriteReg(0x1C, 0x08);     // ACCEL_CONFIG: ±4 g → 8192 LSB/g
}

// Acceleration magnitude in milli-g (−1 on error)
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
//  Motor temperature: 10 kΩ NTC thermistor (B = 3950) read by the ADC.
//  This is the sensor used in the windings of industrial motors.
//  Voltage → temperature conversion with a PRECOMPUTED TABLE + linear
//  interpolation: no floating-point maths (no log/exp on the Cortex-M0+).
//  Table generated offline: adc = 4095·r/(1+r), r = exp(B·(1/T − 1/298.15))
// ============================================================================
#define NTC_T_MIN   (-40)          // °C, first table entry
#define NTC_T_STEP  5              // °C between two entries
static const uint16_t NTC_TABLE[] = {
  3996, 3955, 3900, 3830, 3740, 3629, 3495, 3337, 3156, 2955, 2738, 2510,
  2278, 2048, 1825, 1614, 1419, 1241, 1081,  940,  815,  707,  613,  532,
   462,  401,  350,  305,  267,  234,  206,  181,  160,  142 };   // −40 … 125 °C
#define NTC_N (sizeof(NTC_TABLE) / sizeof(NTC_TABLE[0]))

// ============================================================================
//  Humidity: Honeywell HIH-4030 analog sensor (ratiometric output)
//    Vout = Vcc × (0.0062 × RH + 0.16)  →  RH = (Vout/Vcc − 0.16) / 0.0062
//  With integers: r = Vout/Vcc in ‰; RH (tenths of %) = (r − 160) × 100 / 62
//  (in the simulator, a potentiometer replaces the sensor)
// ============================================================================
static uint16_t humidityRead() {
  uint32_t sum = 0;
  for (int i = 0; i < 16; i++) sum += analogRead(PIN_HUM);
  int32_t r = (int32_t)((sum / 16) * 1000UL / 4095UL);   // Vout/Vcc ratio in ‰
  int32_t hr = (r - 160) * 100 / 62;                     // tenths of %
  if (hr < 0) hr = 0;
  if (hr > 1000) hr = 1000;
  return (uint16_t)hr;
}

// Returns true if the reading is plausible; temperature in tenths of °C
static bool ntcReadTempD(int16_t *temp_d) {
  uint32_t sum = 0;
  for (int i = 0; i < 16; i++) sum += analogRead(PIN_TEMP);
  uint32_t adc = sum / 16;
  if (adc > NTC_TABLE[0] || adc < NTC_TABLE[NTC_N - 1]) return false;   // sensor missing / out of range
  for (uint32_t i = 0; i + 1 < NTC_N; i++) {
    if (adc <= NTC_TABLE[i] && adc >= NTC_TABLE[i + 1]) {
      int32_t span = NTC_TABLE[i] - NTC_TABLE[i + 1];
      int32_t t = (NTC_T_MIN + (int32_t)i * NTC_T_STEP) * 10
                + (int32_t)(NTC_TABLE[i] - adc) * NTC_T_STEP * 10 / span;
      *temp_d = (int16_t)t;
      return true;
    }
  }
  return false;
}

// ============================================================================
//  VibTask: 100 Hz sampling, RMS of the AC component over 1 s
// ============================================================================
static void VibTask(void *) {
  static int16_t win[VIB_WINDOW];
  uint16_t idx = 0;
  uint32_t n = 0;
  bool ok = g_mpuOk;
  Link.println("# vib task started");
  TickType_t last = xTaskGetTickCount();

  for (;;) {
    vTaskDelayUntil(&last, pdMS_TO_TICKS(VIB_PERIOD_MS));   // strict period

    int32_t s = ok ? mpuReadMagnitudeMg() : 1000;
    if (s < 0) { ok = false; s = 1000; }
    if (g_fault) {
      // Simulated 25 Hz imbalance: sampled at 100 Hz → 4 points per period
      static const int16_t SINE4[4] = { 0, 900, 0, -900 };
      s += SINE4[n & 3];
    }
    win[idx++] = (int16_t)constrain(s, -32000, 32000);
    n++;

    if (idx >= VIB_WINDOW) {
      idx = 0;
      int32_t mean = 0;
      for (int i = 0; i < VIB_WINDOW; i++) mean += win[i];
      mean /= VIB_WINDOW;                            // removes gravity/tilt
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
//  EnvTask: NTC temperature + HIH-4030 humidity + ACS712 current (ADC), 500 ms
// ============================================================================
static void EnvTask(void *) {
  Link.println("# env task started");
  for (;;) {
    uint32_t sum = 0;
    for (int i = 0; i < 32; i++) sum += analogRead(PIN_CURRENT);
    int32_t mv = (int32_t)((sum / 32) * 3300UL / 4095UL);
    int32_t ma = (mv - ACS_ZERO_MV) * 1000L / ACS_SENS_UV_MA;
    if (ma < 0) ma = -ma;
    if (g_fault) ma += 2500;                         // simulated overcurrent
    if (ma > 65535) ma = 65535;

    int16_t t = 0;
    bool tempOk = ntcReadTempD(&t);
    uint16_t hum = humidityRead();

    xSemaphoreTake(measureMutex, portMAX_DELAY);
    g_measure.curr_ma = (uint16_t)ma;
    g_measure.dht_ok  = tempOk;                      // "temperature sensor OK" bit
    if (tempOk) g_measure.temp_d = (int16_t)(t + (g_fault ? 350 : 0));
    g_measure.hum_d   = hum;
    xSemaphoreGive(measureMutex);

    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

// ============================================================================
//  BuzzerTask: 300 ms beep at ~500 Hz when notified
//  (square wave generated by the task: no hardware timer needed)
// ============================================================================
static void BuzzerTask(void *) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    for (int i = 0; i < 300; i++) {
      digitalWrite(PIN_BUZZER, (i & 1) ? HIGH : LOW);
      vTaskDelay(1);                                 // 1 ms → 2 ms period
    }
    digitalWrite(PIN_BUZZER, LOW);
  }
}

// ============================================================================
//  AnalysisTask: alert levels, LEDs/buzzer, frame building
// ============================================================================
static Level levelOf(int32_t v, int32_t warn, int32_t crit) {
  return v >= crit ? LVL_CRITICAL : (v >= warn ? LVL_WARNING : LVL_NORMAL);
}

static void AnalysisTask(void *) {
  uint16_t seq = 0;
  Link.println("# analysis task started");
  TickType_t last = xTaskGetTickCount();

  for (;;) {
    vTaskDelayUntil(&last, pdMS_TO_TICKS(1000));

    // Button press signalled by the ISR? (non-blocking check)
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

    // Queue full → replace the oldest entry (fresh data wins)
    if (xQueueSend(frameQueue, &f, 0) != pdTRUE) {
      proto_frame_t old;
      xQueueReceive(frameQueue, &old, 0);
      xQueueSend(frameQueue, &f, 0);
    }
  }
}

// ============================================================================
//  CommTask: sends the frames on USART2 + receives commands
//    "F1" = fault ON, "F0" = fault OFF (sent by the ESP32 / the gateway)
// ============================================================================
static void CommTask(void *) {
  char buf[PROTO_MAX_FRAME];
  char prev = 0;
  proto_frame_t f;
  Link.println("# comm task started");
  for (;;) {
    if (xQueueReceive(frameQueue, &f, pdMS_TO_TICKS(20)) == pdTRUE) {
      size_t n = proto_encode(&f, buf, sizeof buf);
      if (n) Link.write((const uint8_t *)buf, n);
    }
    while (Link.available()) {
      char c = (char)Link.read();
      if (prev == 'F' && c == '1') { g_fault = true;  Link.println("# fault ON"); }
      if (prev == 'F' && c == '0') { g_fault = false; Link.println("# fault OFF"); }
      prev = c;
    }
  }
}

// ============================================================================
//  Button ISR: short, it only wakes up the analysis task
// ============================================================================
static void onButton() {
  uint32_t now = millis();
  if (now - g_lastIsrMs < 200) return;               // debounce
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
  digitalWrite(PIN_LED_OK, HIGH);                    // immediate sign of life
  analogReadResolution(12);

  // Sensors initialised BEFORE FreeRTOS (easier to troubleshoot)
  i2cBegin();
  g_mpuOk = mpuInit();
  Link.println(g_mpuOk ? "# MPU6050 OK" : "# MPU6050 missing (vibration disabled)");
  Link.println("# NTC + HIH-4030 humidity sensors ready");

  measureMutex = xSemaphoreCreateMutex();            // mutex with priority inheritance
  frameQueue   = xQueueCreate(4, sizeof(proto_frame_t));

  // Stacks in 4-byte words (12 KB of RAM in total on this microcontroller)
  BaseType_t ok = pdTRUE;
  ok &= xTaskCreate(VibTask,      "vib",      160, NULL, tskIDLE_PRIORITY + 4, NULL);
  ok &= xTaskCreate(EnvTask,      "env",      200, NULL, tskIDLE_PRIORITY + 3, NULL);
  ok &= xTaskCreate(AnalysisTask, "analysis", 160, NULL, tskIDLE_PRIORITY + 3, &analysisHandle);
  ok &= xTaskCreate(CommTask,     "comm",     160, NULL, tskIDLE_PRIORITY + 3, NULL);
  ok &= xTaskCreate(BuzzerTask,   "buzzer",    96, NULL, tskIDLE_PRIORITY + 1, &buzzerHandle);
  Link.println(ok == pdTRUE ? "# tasks created, starting FreeRTOS" : "# ERROR creating tasks (memory)");

  attachInterrupt(digitalPinToInterrupt(PIN_BUTTON), onButton, FALLING);

  vTaskStartScheduler();                             // never returns
  Link.println("# ERROR: not enough memory for FreeRTOS");
}

void loop() {
  // Never executed: the FreeRTOS scheduler has taken over.
}
