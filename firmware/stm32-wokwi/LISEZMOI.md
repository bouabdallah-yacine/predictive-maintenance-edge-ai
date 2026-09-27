# STM32 Blue Pill (STM32F103C8) + FreeRTOS — simulation Wokwi

Nœud d'acquisition industriel : 4 tâches FreeRTOS lisent les capteurs (I2C, ADC, GPIO),
calculent la vibration RMS, décident du niveau d'alerte et envoient une trame UART par seconde.

## Lancer
1. Ouvrir **ce dossier** dans VS Code (Fichier → Ouvrir le dossier).
2. PlatformIO → **Build** (1er build : télécharge le compilateur ARM, ~5 min).
3. **F1 → Wokwi: Start Simulator**.
4. Dans le dossier `backend` : `npm run stm32` → les trames partent en MQTT.
5. Le dashboard affiche la machine **stm32-01**.

## Brochage
| Élément | Broche STM32 | Périphérique |
|---|---|---|
| DHT22 | PA1 | GPIO (timing µs) |
| MPU6050 | PB6 SCL / PB7 SDA | I2C1 |
| Potentiomètre (ACS712) | PA0 | ADC1 canal 0 |
| LEDs vert/jaune/rouge | PB12 / PB13 / PB14 | GPIO |
| Buzzer | PB15 | Timer (tone) |
| Bouton « Panne » | PB11 | EXTI (interruption) |
| Liaison ESP32 | PA9 TX / PA10 RX | USART1 115200 bauds |

## Tâches FreeRTOS
| Tâche | Priorité | Période | Rôle |
|---|---|---|---|
| VibTask | 4 | 10 ms | lit le MPU6050, RMS sur 1 s |
| EnvTask | 3 | 2 s | DHT22 + courant (moyenne 32 échantillons ADC) |
| AnalysisTask | 3 | 1 s | niveaux, LEDs, buzzer, trame → file |
| CommTask | 2 | événement | envoie les trames, reçoit les commandes F1/F0 |

Synchronisation : mutex sur les mesures partagées, file de messages vers CommTask,
notification de tâche depuis l'ISR du bouton.
