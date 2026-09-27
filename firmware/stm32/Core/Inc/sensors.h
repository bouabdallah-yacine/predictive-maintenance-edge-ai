/**
 * @file  sensors.h
 * @brief Pilotes capteurs : MPU6050 (I2C), DHT22 (1-Wire propriétaire), ACS712 (ADC).
 */
#ifndef SENSORS_H
#define SENSORS_H

#include <stdint.h>
#include "main.h"

typedef struct { int16_t ax, ay, az; } mpu_raw_t;

HAL_StatusTypeDef MPU6050_Init(I2C_HandleTypeDef *hi2c);
HAL_StatusTypeDef MPU6050_ReadAccel(I2C_HandleTypeDef *hi2c, mpu_raw_t *out);
/** Convertit une mesure brute (±4 g → 8192 LSB/g) en milli-g de norme. */
int32_t           MPU6050_MagnitudeMg(const mpu_raw_t *r);

void              DHT22_Init(void);
/** temp en dixièmes de °C, hum en dixièmes de %. */
HAL_StatusTypeDef DHT22_Read(int16_t *temp_d, uint16_t *hum_d);

/** Lit le courant ACS712 (moyenne de n échantillons ADC), en mA. */
uint16_t          ACS712_ReadmA(ADC_HandleTypeDef *hadc, uint8_t n);

#endif /* SENSORS_H */
