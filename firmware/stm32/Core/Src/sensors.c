/**
 * @file  sensors.c
 * @brief Low-level sensor drivers (STM32 HAL).
 */
#include "sensors.h"
#include "app_config.h"
#include "FreeRTOS.h"
#include "task.h"
#include <math.h>

/* ------------------------------------------------------------------------ */
/*  MPU6050 — I2C accelerometer                                             */
/* ------------------------------------------------------------------------ */
#define MPU_ADDR          (0x68 << 1)
#define MPU_WHO_AM_I      0x75
#define MPU_PWR_MGMT_1    0x6B
#define MPU_SMPLRT_DIV    0x19
#define MPU_CONFIG        0x1A
#define MPU_ACCEL_CONFIG  0x1C
#define MPU_ACCEL_XOUT_H  0x3B
#define MPU_LSB_PER_G     8192      /* ±4 g range */

HAL_StatusTypeDef MPU6050_Init(I2C_HandleTypeDef *hi2c)
{
  uint8_t id = 0, v;
  if (HAL_I2C_Mem_Read(hi2c, MPU_ADDR, MPU_WHO_AM_I, 1, &id, 1, 50) != HAL_OK || id != 0x68)
    return HAL_ERROR;
  v = 0x00; HAL_I2C_Mem_Write(hi2c, MPU_ADDR, MPU_PWR_MGMT_1,   1, &v, 1, 50);  /* wake up */
  v = 0x07; HAL_I2C_Mem_Write(hi2c, MPU_ADDR, MPU_SMPLRT_DIV,   1, &v, 1, 50);  /* 1 kHz */
  v = 0x03; HAL_I2C_Mem_Write(hi2c, MPU_ADDR, MPU_CONFIG,       1, &v, 1, 50);  /* DLPF 44 Hz */
  v = 0x08; HAL_I2C_Mem_Write(hi2c, MPU_ADDR, MPU_ACCEL_CONFIG, 1, &v, 1, 50);  /* ±4 g */
  return HAL_OK;
}

HAL_StatusTypeDef MPU6050_ReadAccel(I2C_HandleTypeDef *hi2c, mpu_raw_t *out)
{
  uint8_t d[6];
  HAL_StatusTypeDef st = HAL_I2C_Mem_Read(hi2c, MPU_ADDR, MPU_ACCEL_XOUT_H, 1, d, 6, 10);
  if (st != HAL_OK) return st;
  out->ax = (int16_t)((d[0] << 8) | d[1]);
  out->ay = (int16_t)((d[2] << 8) | d[3]);
  out->az = (int16_t)((d[4] << 8) | d[5]);
  return HAL_OK;
}

int32_t MPU6050_MagnitudeMg(const mpu_raw_t *r)
{
  float x = r->ax, y = r->ay, z = r->az;
  return (int32_t)(sqrtf(x * x + y * y + z * z) * 1000.0f / MPU_LSB_PER_G);
}

/* ------------------------------------------------------------------------ */
/*  DHT22 — proprietary 1-wire protocol, µs timing via the DWT counter      */
/*  Pin configured in CubeMX: GPIO_Output Open-Drain + pull-up.             */
/* ------------------------------------------------------------------------ */
static inline void dwt_init(void)
{
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

static inline uint32_t micros_cycles(uint32_t us) { return us * (SystemCoreClock / 1000000U); }

static inline void delay_us(uint32_t us)
{
  uint32_t start = DWT->CYCCNT, n = micros_cycles(us);
  while ((DWT->CYCCNT - start) < n) {}
}

/* Waits for the pin to reach `level`; returns the duration in µs or -1 */
static int32_t wait_level(GPIO_PinState level, uint32_t timeout_us)
{
  uint32_t start = DWT->CYCCNT, max = micros_cycles(timeout_us);
  while (HAL_GPIO_ReadPin(DHT22_GPIO_Port, DHT22_Pin) != level) {
    if ((DWT->CYCCNT - start) > max) return -1;
  }
  return (int32_t)((DWT->CYCCNT - start) / (SystemCoreClock / 1000000U));
}

void DHT22_Init(void)
{
  dwt_init();
  HAL_GPIO_WritePin(DHT22_GPIO_Port, DHT22_Pin, GPIO_PIN_SET);   /* line released */
}

HAL_StatusTypeDef DHT22_Read(int16_t *temp_d, uint16_t *hum_d)
{
  uint8_t data[5] = {0};

  /* Start signal: line held low for ≥ 1 ms */
  HAL_GPIO_WritePin(DHT22_GPIO_Port, DHT22_Pin, GPIO_PIN_RESET);
  vTaskDelay(pdMS_TO_TICKS(2));
  HAL_GPIO_WritePin(DHT22_GPIO_Port, DHT22_Pin, GPIO_PIN_SET);

  /* ≈ 5 ms critical section: µs timing cannot tolerate any preemption */
  taskENTER_CRITICAL();
  HAL_StatusTypeDef st = HAL_ERROR;
  if (wait_level(GPIO_PIN_RESET, 100) < 0) goto out;   /* sensor response: 80 µs low */
  if (wait_level(GPIO_PIN_SET,   100) < 0) goto out;   /* then 80 µs high */
  if (wait_level(GPIO_PIN_RESET, 100) < 0) goto out;

  for (int i = 0; i < 40; i++) {
    if (wait_level(GPIO_PIN_SET, 70) < 0) goto out;     /* 50 µs low */
    int32_t high = wait_level(GPIO_PIN_RESET, 100);     /* 26 µs = 0, 70 µs = 1 */
    if (high < 0) goto out;
    data[i / 8] = (uint8_t)((data[i / 8] << 1) | (high > 45 ? 1 : 0));
  }
  st = HAL_OK;
out:
  taskEXIT_CRITICAL();
  if (st != HAL_OK) return st;

  if ((uint8_t)(data[0] + data[1] + data[2] + data[3]) != data[4]) return HAL_ERROR;

  *hum_d = (uint16_t)((data[0] << 8) | data[1]);
  int16_t t = (int16_t)(((data[2] & 0x7F) << 8) | data[3]);
  *temp_d = (data[2] & 0x80) ? -t : t;
  return HAL_OK;
}

/* ------------------------------------------------------------------------ */
/*  ACS712 — Hall-effect current sensor, read through the ADC               */
/* ------------------------------------------------------------------------ */
uint16_t ACS712_ReadmA(ADC_HandleTypeDef *hadc, uint8_t n)
{
  uint32_t sum = 0, count = 0;
  for (uint8_t i = 0; i < n; i++) {
    HAL_ADC_Start(hadc);
    if (HAL_ADC_PollForConversion(hadc, 2) == HAL_OK) {
      sum += HAL_ADC_GetValue(hadc);
      count++;
    }
  }
  HAL_ADC_Stop(hadc);
  if (count == 0) return 0;

  int32_t mv = (int32_t)((sum / count) * ADC_VREF_MV / ADC_MAX);
  int32_t ma = (mv - ACS_ZERO_MV) * 1000 / ACS_SENS_UV_PER_MA;
  if (ma < 0) ma = -ma;                         /* alternating current / reversed direction */
  return (uint16_t)(ma > 65535 ? 65535 : ma);
}
