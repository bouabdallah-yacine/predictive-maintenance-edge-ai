# 🤖 On-device AI (TinyML): anomaly detection on the ESP32

A small **neural network (4 → 16 → 16 → 1, 369 weights, ≈ 1.5 KB)** runs directly
on the ESP32 and outputs, for each measurement, an **anomaly score** between 0 and 1.

## Why, on top of thresholds?

Thresholds look at each quantity **separately**. The AI looks at the **combination**:

| Situation | Thresholds (60 °C / 0.3 g / 3.5 A) | On-device AI |
|---|---|---|
| 45 °C while the motor draws no current (cooling failure) | ✅ normal | 🚨 anomaly (temperature) |
| 15 °C while the motor draws 3 A (faulty sensor) | ✅ normal | 🚨 anomaly (temperature) |
| Vibration peak = 5 × RMS (shocks: damaged bearing) | ✅ normal | 🚨 anomaly (vibration) |
| Normal operation (24 °C when stopped, 40 °C at 2.5 A…) | ✅ normal | ✅ normal |

## How it works

1. **Data**: a physical model of the machine generates normal-operation measurements
   (heating ∝ current², load-dependent vibration, healthy crest factor).
2. **Training** (`train_model.py`, NumPy only): the network learns to tell these normal
   measurements apart from randomly drawn ones, so it learns the *shape* of the normal zone.
   Threshold: 0.5 → **0.04 % false alarms** on unseen normal data.
3. **Export**: the weights are written as C code in `tinyml_model.h`.
4. **Inference** (`tinyml.h`, plain C, no library): ≈ 600 multiplications per measurement.
   Debounce (3 consecutive measurements) and **explanation**: the quantity which, brought
   back to a normal value, lowers the score the most is reported as the "probable cause".
5. The verdict is sent over MQTT (`"ai": {"score", "anomaly", "cause"}`) → alert in the
   dashboard and on Telegram.

## Commands

```bash
pip install numpy
python ai/train_model.py                                    # trains and copies the model into the firmwares
gcc -O2 -Wall -I ai ai/test_tinyml.c -lm -o t && ./t        # checks: C == Python
```
