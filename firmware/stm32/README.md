# STM32 firmware — FreeRTOS (CMSIS-RTOS v2)

Acquisition node: reads the sensors, computes the RMS vibration, determines the alert level
and sends a UART frame every second to the ESP32 (or to the Python gateway).

```
Core/
├── Inc/app_config.h   thresholds, periods, pinout, SIMULATED_SENSORS
├── Inc/protocol.h     UART frame format ($MM,...*CS)
├── Inc/sensors.h
├── Src/app_tasks.c    4 FreeRTOS tasks + button ISR
├── Src/protocol.c     encoder + state-machine parser (plain C, tested on a PC)
└── Src/sensors.c      MPU6050 (I2C), DHT22 (DWT timing), ACS712 (ADC)
test/test_protocol.c   protocol unit tests
renode/                Renode simulation script
```

## Integrating into STM32CubeIDE

1. **New STM32 Project** → board **STM32F4DISCOVERY** (for Renode) or **NUCLEO-F401RE** (real board).
2. In the `.ioc` (CubeMX):
   | Peripheral | Setting |
   |---|---|
   | SYS | Timebase Source = **TIM6** (SysTick is used by FreeRTOS) |
   | USART1 | Asynchronous 115200 8N1 (PA9 TX / PA10 RX) → to the ESP32 |
   | USART2 | Asynchronous 115200 8N1 (debug console) |
   | I2C1 | Fast mode 400 kHz (PB8 SCL / PB9 SDA) → MPU6050 |
   | ADC1 | IN0 (PA0) 12 bits → ACS712 |
   | GPIO | PA1 Output **Open-Drain** + Pull-up (DHT22); PB0/PB1/PB2 LEDs; PB10 buzzer; PC13 EXTI falling (button) |
   | FREERTOS | **CMSIS_V2** interface, `TOTAL_HEAP_SIZE` ≥ 15,000 |
3. Copy `Core/Inc/*` and `Core/Src/*` into the project.
4. In `main.c`:
   ```c
   /* USER CODE BEGIN Includes */
   void App_Init(void);
   /* USER CODE END Includes */
   ...
   /* USER CODE BEGIN RTOS_THREADS */
   App_Init();
   /* USER CODE END RTOS_THREADS */
   ```
5. Delete the generated `defaultTask` (or leave it empty).
6. Build. `SIMULATED_SENSORS` defaults to 1 (generated data); set it to 0 with real sensors.

## Simulating with Renode

Renode (free, https://renode.io) runs the **real** ARM binary: FreeRTOS, UART, timers…
It does not model the MPU6050 or the DHT22, hence the `SIMULATED_SENSORS=1` mode.

```bash
renode renode/machine_monitor.resc          # from the CubeIDE project folder
python ../../gateway/uart_gateway.py --tcp localhost:3456
```

The gateway reads the frames on TCP port 3456 and publishes them over MQTT, just as the ESP32 would.
With real hardware: `python uart_gateway.py --serial COM5` (or `/dev/ttyACM0`).

## Testing the protocol on a PC

```bash
cd test
gcc -Wall -Wextra -I../Core/Inc test_protocol.c ../Core/Src/protocol.c -o test_protocol && ./test_protocol
```

## Technical points worth explaining in an interview

- `osDelayUntil` → strict sampling period (no drift, unlike `osDelay`).
- Mutex with **priority inheritance** on the shared measurements (avoids priority inversion).
- The button ISR only calls `osThreadFlagsSet`: processing is deferred to the task.
- Short critical section (~5 ms) only while reading the DHT22 (µs timing via DWT->CYCCNT).
- Message queue: when it is full, the oldest frame is overwritten (fresh data wins).
- Text frame with XOR checksum and integers (no `printf("%f")` with newlib-nano).
