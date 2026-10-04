# STM32 Nucleo-C031C6 (Cortex-M0+) + FreeRTOS — simulation Wokwi

Nœud d'acquisition industriel : 4 tâches FreeRTOS lisent les capteurs (I2C, ADC, GPIO),
calculent la vibration RMS (en entiers, pas de FPU sur Cortex-M0+), décident du niveau
d'alerte et envoient une trame UART par seconde.

## Lancer
1. Ouvrir **ce dossier** dans VS Code (Fichier → Ouvrir le dossier).
2. PlatformIO → **Build**.
3. **F1 → Wokwi: Start Simulator**.
4. Dans le dossier `backend` : `npm run stm32` → les trames partent en MQTT.
5. Le dashboard affiche la machine **stm32-01**.

## Brochage
| Élément | Broche STM32 | Périphérique |
|---|---|---|
| Thermistance NTC 10 kΩ (température moteur) | PA1 | ADC canal 1 (table précalculée) |
| MPU6050 | PB8 SCL / PB9 SDA | I2C1 |
| Potentiomètre (ACS712) | PA0 | ADC canal 0 |
| LEDs vert/jaune/rouge | PB13 / PB14 / PB15 | GPIO |
| Buzzer | PB10 | Timer (tone) |
| Bouton « Panne » | PB11 | EXTI (interruption) |
| Liaison ESP32 / passerelle | PA2 TX / PA3 RX | USART2 115200 bauds |

## Tâches FreeRTOS
| Tâche | Priorité | Période | Rôle |
|---|---|---|---|
| VibTask | 4 | 10 ms | lit le MPU6050, RMS sur 1 s |
| EnvTask | 3 | 500 ms | température NTC + courant (moyennes ADC) |
| AnalysisTask | 3 | 1 s | niveaux, LEDs, buzzer, trame → file |
| CommTask | 3 | événement | envoie les trames, reçoit les commandes F1/F0 |

Synchronisation : mutex sur les mesures partagées, file de messages vers CommTask,
notification de tâche depuis l'ISR du bouton.

Pourquoi la Nucleo-C031C6 et pas la Blue Pill ? Dans le simulateur Wokwi, le démarrage
de FreeRTOS sur la Blue Pill (Cortex-M3) bloquait ; la Nucleo C031 (Cortex-M0+) est la
carte STM32 utilisée par les projets FreeRTOS de référence sur Wokwi.

Pourquoi une NTC plutôt qu'un DHT22 sur ce nœud ? Le DHT22 utilise un protocole à
timing microseconde qui échouait aléatoirement dans le simulateur sur cette carte.
La thermistance NTC, lue par l'ADC, est aussi plus réaliste : c'est la sonde montée
dans les bobinages des moteurs industriels. (Le nœud ESP32 garde son DHT22.)
