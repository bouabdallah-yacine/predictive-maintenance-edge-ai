/**
 * @file  app_tasks.c
 * @brief Application FreeRTOS (CMSIS-RTOS v2) du nœud d'acquisition STM32.
 *
 *  ┌──────────────┐ 100 Hz  ┌──────────────┐
 *  │ VibTask (P5) │───────► │              │
 *  └──────────────┘ mutex   │  g_measure   │ 1 Hz  ┌─────────────────┐  queue  ┌───────────────┐ UART1
 *  ┌──────────────┐ 0,5 Hz  │  (partagé)   │─────► │ AnalysisTask(P4)│───────► │ CommTask (P3) │──────► ESP32
 *  │ EnvTask (P4) │───────► │              │       └───────┬─────────┘         └───────────────┘
 *  └──────────────┘         └──────────────┘               │ LEDs / buzzer
 *  Bouton B1 (EXTI) ──► thread flag ──► AnalysisTask (bascule panne simulée)
 *
 *  Intégration dans un projet CubeMX :
 *   - Middleware FREERTOS, interface CMSIS_V2 ; Timebase HAL = TIM (pas SysTick)
 *   - Dans main.c, USER CODE BEGIN RTOS_THREADS :  App_Init();
 *   - Dans main.c, USER CODE BEGIN 4 : rediriger HAL_GPIO_EXTI_Callback (déjà ici)
 */
#include "main.h"
#include "cmsis_os2.h"
#include "app_config.h"
#include "protocol.h"
#include "sensors.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

extern I2C_HandleTypeDef  hi2c1;
extern ADC_HandleTypeDef  hadc1;
extern UART_HandleTypeDef huart1;   /* liaison vers l'ESP32 */
extern UART_HandleTypeDef huart2;   /* console de debug (ST-Link VCP) */

/* ------------------------------------------------------------------------ */
typedef struct {
  int16_t  temp_d;
  uint16_t hum_d;
  uint16_t vib_rms_mg;
  uint16_t vib_peak_mg;
  uint16_t curr_ma;
  uint8_t  dht_ok, mpu_ok;
} measure_t;

typedef enum { LVL_NORMAL = 0, LVL_WARNING, LVL_CRITICAL } level_t;

static measure_t          g_measure;
static osMutexId_t        measureMutex;
static osMessageQueueId_t frameQueue;
static osThreadId_t       analysisTaskHandle;
static volatile uint8_t   g_fault = 0;

#define FLAG_BUTTON  0x0001u

static const osThreadAttr_t vibAttr      = { .name = "vib",      .stack_size = 512 * 4, .priority = osPriorityAboveNormal1 };
static const osThreadAttr_t envAttr      = { .name = "env",      .stack_size = 512 * 4, .priority = osPriorityAboveNormal };
static const osThreadAttr_t analysisAttr = { .name = "analysis", .stack_size = 512 * 4, .priority = osPriorityAboveNormal };
static const osThreadAttr_t commAttr     = { .name = "comm",     .stack_size = 512 * 4, .priority = osPriorityNormal };

/* ------------------------------------------------------------------------ */
/*  Génération de données simulées (Renode / démo sans capteurs)            */
/* ------------------------------------------------------------------------ */
#if SIMULATED_SENSORS
static float frand(void) { return (float)rand() / (float)RAND_MAX - 0.5f; }
#endif

/* ------------------------------------------------------------------------ */
/*  VibTask : échantillonnage 100 Hz, RMS + crête sur fenêtre de 1 s        */
/* ------------------------------------------------------------------------ */
static void VibTask(void *arg)
{
  (void)arg;
  static int16_t win[VIB_WINDOW];
  uint16_t idx = 0;
  uint32_t n = 0;
  uint8_t ok = 0;

#if !SIMULATED_SENSORS
  ok = (MPU6050_Init(&hi2c1) == HAL_OK);
#else
  ok = 1;
#endif

  uint32_t next = osKernelGetTickCount();
  for (;;) {
    next += VIB_PERIOD_MS;
    osDelayUntil(next);                      /* période stricte, sans dérive */

    int32_t dyn_mg;
#if SIMULATED_SENSORS
    dyn_mg = (int32_t)(40.0f * frand());
#else
    mpu_raw_t raw;
    if (MPU6050_ReadAccel(&hi2c1, &raw) != HAL_OK) { ok = 0; continue; }
    dyn_mg = MPU6050_MagnitudeMg(&raw) - 1000;  /* retire la gravité */
#endif
    if (g_fault) {                             /* balourd simulé à 25 Hz */
      dyn_mg += (int32_t)(900.0f * sinf(2.0f * 3.14159265f * 25.0f * (float)n / 100.0f));
    }
    win[idx++] = (int16_t)dyn_mg;
    n++;

    if (idx >= VIB_WINDOW) {
      idx = 0;
      /* RMS de la composante alternative (moyenne de la fenêtre retirée) */
      int32_t mean = 0;
      for (int i = 0; i < VIB_WINDOW; i++) mean += win[i];
      mean /= VIB_WINDOW;
      uint64_t sumsq = 0;
      int32_t peak = 0;
      for (int i = 0; i < VIB_WINDOW; i++) {
        int32_t d = win[i] - mean;
        sumsq += (uint64_t)((int64_t)d * d);
        if (abs(d) > peak) peak = abs(d);
      }
      osMutexAcquire(measureMutex, osWaitForever);
      g_measure.vib_rms_mg  = (uint16_t)sqrtf((float)sumsq / VIB_WINDOW);
      g_measure.vib_peak_mg = (uint16_t)peak;
      g_measure.mpu_ok = ok;
      osMutexRelease(measureMutex);
    }
  }
}

/* ------------------------------------------------------------------------ */
/*  EnvTask : DHT22 (toutes les 2 s) + courant ACS712                        */
/* ------------------------------------------------------------------------ */
static void EnvTask(void *arg)
{
  (void)arg;
#if !SIMULATED_SENSORS
  DHT22_Init();
#else
  float temp = 38.0f;
#endif
  for (;;) {
    int16_t t = 0; uint16_t h = 0, ma = 0; uint8_t ok;
#if SIMULATED_SENSORS
    temp += ((g_fault ? 90.0f : 40.0f) - temp) * 0.1f + frand() * 0.3f;
    t = (int16_t)(temp * 10.0f);
    h = (uint16_t)(450 + frand() * 20);
    ma = (uint16_t)(1600 + frand() * 100 + (g_fault ? 3200 : 0));
    ok = 1;
#else
    ok = (DHT22_Read(&t, &h) == HAL_OK);
    ma = ACS712_ReadmA(&hadc1, 32);
    if (g_fault) { t += 350; ma += 2500; }
#endif
    osMutexAcquire(measureMutex, osWaitForever);
    if (ok) { g_measure.temp_d = t; g_measure.hum_d = h; }
    g_measure.dht_ok  = ok;
    g_measure.curr_ma = ma;
    osMutexRelease(measureMutex);

    osDelay(ENV_PERIOD_MS);
  }
}

/* ------------------------------------------------------------------------ */
/*  AnalysisTask : niveaux d'alerte, actionneurs, construction de la trame   */
/* ------------------------------------------------------------------------ */
static level_t level_of(int32_t v, int32_t warn, int32_t crit)
{
  return v >= crit ? LVL_CRITICAL : (v >= warn ? LVL_WARNING : LVL_NORMAL);
}

static void set_outputs(level_t lvl)
{
  HAL_GPIO_WritePin(LED_OK_GPIO_Port,    LED_OK_Pin,    lvl == LVL_NORMAL   ? GPIO_PIN_SET : GPIO_PIN_RESET);
  HAL_GPIO_WritePin(LED_WARN_GPIO_Port,  LED_WARN_Pin,  lvl == LVL_WARNING  ? GPIO_PIN_SET : GPIO_PIN_RESET);
  HAL_GPIO_WritePin(LED_ALARM_GPIO_Port, LED_ALARM_Pin, lvl == LVL_CRITICAL ? GPIO_PIN_SET : GPIO_PIN_RESET);
  HAL_GPIO_WritePin(BUZZER_GPIO_Port,    BUZZER_Pin,    lvl == LVL_CRITICAL ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static void AnalysisTask(void *arg)
{
  (void)arg;
  uint16_t seq = 0;
  uint32_t next = osKernelGetTickCount();

  for (;;) {
    /* Attend soit l'échéance de 1 s, soit un appui bouton */
    next += REPORT_PERIOD_MS;
    int32_t wait = (int32_t)(next - osKernelGetTickCount());
    uint32_t flags = osThreadFlagsWait(FLAG_BUTTON, osFlagsWaitAny, wait > 0 ? (uint32_t)wait : 0);
    if (!(flags & osFlagsError) && (flags & FLAG_BUTTON)) {
      g_fault = !g_fault;
      osDelayUntil(next);              /* garde la cadence de 1 Hz */
    }

    measure_t m;
    osMutexAcquire(measureMutex, osWaitForever);
    m = g_measure;
    osMutexRelease(measureMutex);

    level_t lt = level_of(m.temp_d, TEMP_WARN_D, TEMP_CRIT_D);
    level_t lv = level_of(m.vib_rms_mg, VIB_WARN_MG, VIB_CRIT_MG);
    level_t lc = level_of(m.curr_ma, CURR_WARN_MA, CURR_CRIT_MA);
    level_t g = lt > lv ? lt : lv;
    if (lc > g) g = lc;
    set_outputs(g);

    proto_frame_t f = {
      .seq = seq++, .temp_d = m.temp_d, .hum_d = m.hum_d,
      .vib_mg = m.vib_rms_mg, .peak_mg = m.vib_peak_mg, .curr_ma = m.curr_ma,
      .flags = (uint8_t)((m.dht_ok ? PROTO_FLAG_DHT_OK : 0) |
                         (m.mpu_ok ? PROTO_FLAG_MPU_OK : 0) |
                         (g_fault  ? PROTO_FLAG_FAULT  : 0) |
                         ((uint8_t)g << PROTO_LEVEL_SHIFT)),
    };
    /* File pleine → on écrase la plus ancienne (la donnée fraîche prime) */
    if (osMessageQueuePut(frameQueue, &f, 0, 0) != osOK) {
      proto_frame_t old;
      osMessageQueueGet(frameQueue, &old, NULL, 0);
      osMessageQueuePut(frameQueue, &f, 0, 0);
    }
  }
}

/* ------------------------------------------------------------------------ */
/*  CommTask : sérialisation + envoi UART vers l'ESP32 (et copie debug)      */
/* ------------------------------------------------------------------------ */
static void CommTask(void *arg)
{
  (void)arg;
  char buf[PROTO_MAX_FRAME];
  proto_frame_t f;
  for (;;) {
    if (osMessageQueueGet(frameQueue, &f, NULL, osWaitForever) == osOK) {
      size_t n = proto_encode(&f, buf, sizeof buf);
      if (n) {
        HAL_UART_Transmit(&huart1, (uint8_t *)buf, (uint16_t)n, 50);
        HAL_UART_Transmit(&huart2, (uint8_t *)buf, (uint16_t)n, 50);
      }
    }
  }
}

/* ------------------------------------------------------------------------ */
/*  Interruption bouton : ne fait que signaler la tâche (ISR courte)         */
/* ------------------------------------------------------------------------ */
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
  static uint32_t last = 0;
  if (GPIO_Pin == FAULT_BTN_Pin && analysisTaskHandle) {
    uint32_t now = HAL_GetTick();
    if (now - last > 200) {                        /* anti-rebond */
      osThreadFlagsSet(analysisTaskHandle, FLAG_BUTTON);
      last = now;
    }
  }
}

/* ------------------------------------------------------------------------ */
void App_Init(void)
{
  const osMutexAttr_t mAttr = { .name = "measure", .attr_bits = osMutexPrioInherit };
  measureMutex = osMutexNew(&mAttr);                 /* héritage de priorité */
  frameQueue   = osMessageQueueNew(8, sizeof(proto_frame_t), NULL);

  osThreadNew(VibTask, NULL, &vibAttr);
  osThreadNew(EnvTask, NULL, &envAttr);
  analysisTaskHandle = osThreadNew(AnalysisTask, NULL, &analysisAttr);
  osThreadNew(CommTask, NULL, &commAttr);

  const char *hello = "\r\n[STM32] Machine Monitor - FreeRTOS demarre\r\n";
  HAL_UART_Transmit(&huart2, (uint8_t *)hello, (uint16_t)strlen(hello), 50);
}
