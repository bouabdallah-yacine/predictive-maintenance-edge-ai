/**
 * @file  sensors.h
 * @brief Sensor drivers: MPU6050 (I2C), DHT22 (proprietary 1-wire), ACS712 (ADC).
 */
#ifndef SENSORS_H
#define SENSORS_H

#include <stdint.h>
#include "main.h"

typedef struct { int16_t ax, ay, az; } mpu_raw_t;

HAL_StatusTypeDef MPU6050_Init(I2C_HandleTypeDef *hi2c);
HAL_StatusTypeDef MPU6050_ReadAccel(I2C_HandleTypeDef *hi2c, mpu_raw_t *out);
/** Converts a raw reading (±4 g → 8192 LSB/g) into a magnitude in milli-g. */
int32_t           MPU6050_MagnitudeMg(const mpu_raw_t *r);

void              DHT22_Init(void);
/** temp in tenths of °C, hum in tenths of %. */
HAL_StatusTypeDef DHT22_Read(int16_t *temp_d, uint16_t *hum_d);

/** Reads the ACS712 current (average of n ADC samples), in mA. */
uint16_t          ACS712_ReadmA(ADC_HandleTypeDef *hadc, uint8_t n);

#endif /* SENSORS_H */
