# STM32 Nucleo-C031C6 (Cortex-M0+) + FreeRTOS — Wokwi simulation

Industrial acquisition node: 4 FreeRTOS tasks read the sensors (I2C, ADC, GPIO),
compute the RMS vibration (with integers, no FPU on the Cortex-M0+), decide the alert
level and send one UART frame per second.

## Running
1. Open **this folder** in VS Code (File → Open Folder).
2. PlatformIO → **Build**.
3. **F1 → Wokwi: Start Simulator**.
4. In the `backend` folder: `npm run stm32` → the frames are published over MQTT.
5. The dashboard shows the **stm32-01** machine.

## Pinout
| Component | STM32 pin | Peripheral |
|---|---|---|
| 10 kΩ NTC thermistor (motor temperature) | PA1 | ADC channel 1 (precomputed table) |
| HIH-4030 humidity (potentiometer in simulation) | PA4 | ADC channel 4 |
| MPU6050 | PB8 SCL / PB9 SDA | I2C1 |
| Potentiometer (ACS712) | PA0 | ADC channel 0 |
| Green/yellow/red LEDs | PB13 / PB14 / PB15 | GPIO |
| Buzzer | PB10 | Timer (tone) |
| "Fault" button | PB11 | EXTI (interrupt) |
| ESP32 / gateway link | PA2 TX / PA3 RX | USART2 115200 baud |

## FreeRTOS tasks
| Task | Priority | Period | Role |
|---|---|---|---|
| VibTask | 4 | 10 ms | reads the MPU6050, RMS over 1 s |
| EnvTask | 3 | 500 ms | NTC temperature + humidity + current (ADC averages) |
| AnalysisTask | 3 | 1 s | levels, LEDs, buzzer, frame → queue |
| CommTask | 3 | event | sends the frames, receives the F1/F0 commands |

Synchronisation: mutex on the shared measurements, message queue to CommTask,
task notification from the button ISR.

Why the Nucleo-C031C6 and not the Blue Pill? In the Wokwi simulator, FreeRTOS hung at
startup on the Blue Pill (Cortex-M3); the Nucleo C031 (Cortex-M0+) is the STM32 board
used by the reference FreeRTOS projects on Wokwi.

Why an NTC rather than a DHT22 on this node? The DHT22 uses a protocol with
microsecond timing that failed randomly in the simulator on this board.
The NTC thermistor, read by the ADC, is also more realistic: it is the probe fitted
in the windings of industrial motors. (The ESP32 node keeps its DHT22.)
