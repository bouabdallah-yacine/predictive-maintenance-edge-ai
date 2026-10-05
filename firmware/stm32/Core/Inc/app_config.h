/**
 * @file  app_config.h
 * @brief Machine Monitor application configuration (STM32 + FreeRTOS).
 */
#ifndef APP_CONFIG_H
#define APP_CONFIG_H

/* 1 = simulated sensors (Renode / no hardware), 0 = real sensors */
#ifndef SIMULATED_SENSORS
#define SIMULATED_SENSORS      1
#endif

/* Periods (ms) */
#define VIB_PERIOD_MS          10      /* 100 Hz */
#define VIB_WINDOW             100     /* RMS over 1 s */
#define ENV_PERIOD_MS          2000    /* DHT22: 0.5 Hz max */
#define REPORT_PERIOD_MS       1000

/* Thresholds (same as the ESP32 firmware and the backend) */
#define TEMP_WARN_D            600     /* tenths of °C */
#define TEMP_CRIT_D            750
#define VIB_WARN_MG            300
#define VIB_CRIT_MG            600
#define CURR_WARN_MA           3500
#define CURR_CRIT_MA           4500

/* ACS712-05B powered at 5 V, output scaled down to 3.3 V by a voltage divider */
#define ADC_VREF_MV            3300
#define ADC_MAX                4095
#define ACS_ZERO_MV            1650
#define ACS_SENS_UV_PER_MA     122     /* 185 mV/A × 0.66 = 122 µV/mA */

/* Pinout (Nucleo-F401RE / Discovery F407, adjust in CubeMX) */
#define DHT22_GPIO_Port        GPIOA
#define DHT22_Pin              GPIO_PIN_1
#define LED_OK_GPIO_Port       GPIOB
#define LED_OK_Pin             GPIO_PIN_0
#define LED_WARN_GPIO_Port     GPIOB
#define LED_WARN_Pin           GPIO_PIN_1
#define LED_ALARM_GPIO_Port    GPIOB
#define LED_ALARM_Pin          GPIO_PIN_2
#define BUZZER_GPIO_Port       GPIOB
#define BUZZER_Pin             GPIO_PIN_10
#define FAULT_BTN_Pin          GPIO_PIN_13   /* blue button B1 (PC13) */

#endif /* APP_CONFIG_H */
