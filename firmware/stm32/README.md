# Firmware STM32 — FreeRTOS (CMSIS-RTOS v2)

Nœud d'acquisition : lit les capteurs, calcule la vibration RMS, détermine le niveau d'alerte
et envoie une trame UART toutes les secondes vers l'ESP32 (ou vers la passerelle Python).

```
Core/
├── Inc/app_config.h   seuils, périodes, brochage, SIMULATED_SENSORS
├── Inc/protocol.h     format de trame UART ($MM,...*CS)
├── Inc/sensors.h
├── Src/app_tasks.c    4 tâches FreeRTOS + ISR bouton
├── Src/protocol.c     encodeur + parseur à états (C pur, testé sur PC)
└── Src/sensors.c      MPU6050 (I2C), DHT22 (timing DWT), ACS712 (ADC)
test/test_protocol.c   tests unitaires du protocole
renode/                script de simulation Renode
```

## Intégrer dans STM32CubeIDE

1. **New STM32 Project** → carte **STM32F4DISCOVERY** (pour Renode) ou **NUCLEO-F401RE** (vraie carte).
2. Dans le `.ioc` (CubeMX) :
   | Périphérique | Réglage |
   |---|---|
   | SYS | Timebase Source = **TIM6** (SysTick est pris par FreeRTOS) |
   | USART1 | Asynchrone 115200 8N1 (PA9 TX / PA10 RX) → vers l'ESP32 |
   | USART2 | Asynchrone 115200 8N1 (console debug) |
   | I2C1 | Fast mode 400 kHz (PB8 SCL / PB9 SDA) → MPU6050 |
   | ADC1 | IN0 (PA0) 12 bits → ACS712 |
   | GPIO | PA1 Output **Open-Drain** + Pull-up (DHT22) ; PB0/PB1/PB2 LEDs ; PB10 buzzer ; PC13 EXTI falling (bouton) |
   | FREERTOS | Interface **CMSIS_V2**, `TOTAL_HEAP_SIZE` ≥ 15 000 |
3. Copier `Core/Inc/*` et `Core/Src/*` dans le projet.
4. Dans `main.c` :
   ```c
   /* USER CODE BEGIN Includes */
   void App_Init(void);
   /* USER CODE END Includes */
   ...
   /* USER CODE BEGIN RTOS_THREADS */
   App_Init();
   /* USER CODE END RTOS_THREADS */
   ```
5. Supprimer la tâche `defaultTask` générée (ou la laisser vide).
6. Compiler. `SIMULATED_SENSORS` vaut 1 par défaut (données générées) ; passer à 0 avec les vrais capteurs.

## Simuler avec Renode

Renode (gratuit, https://renode.io) exécute le **vrai** binaire ARM : FreeRTOS, UART, timers…
Il ne modélise pas le MPU6050 ni le DHT22, d'où le mode `SIMULATED_SENSORS=1`.

```bash
renode renode/machine_monitor.resc          # depuis le dossier du projet CubeIDE
python ../../gateway/uart_gateway.py --tcp localhost:3456
```

La passerelle lit les trames sur le port TCP 3456 et les publie en MQTT, comme le ferait l'ESP32.
Avec du vrai matériel : `python uart_gateway.py --serial COM5` (ou `/dev/ttyACM0`).

## Tester le protocole sur PC

```bash
cd test
gcc -Wall -Wextra -I../Core/Inc test_protocol.c ../Core/Src/protocol.c -o test_protocol && ./test_protocol
```

## Points techniques à expliquer en entretien

- `osDelayUntil` → période d'échantillonnage stricte (pas de dérive comme avec `osDelay`).
- Mutex avec **héritage de priorité** sur les mesures partagées (évite l'inversion de priorité).
- L'ISR du bouton ne fait qu'un `osThreadFlagsSet` : traitement différé dans la tâche.
- Section critique courte (~5 ms) uniquement pendant la lecture DHT22 (timing µs via DWT->CYCCNT).
- File de messages : si elle est pleine, on écrase la trame la plus ancienne (la donnée fraîche prime).
- Trame texte avec checksum XOR et entiers (pas de `printf("%f")` avec newlib-nano).
