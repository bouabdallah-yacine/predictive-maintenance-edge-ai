/*
 * ============================================================================
 *  TinyML — on-device anomaly detection (neural network inference)
 * ============================================================================
 *  4 → 16 → 16 → 1 network trained by ai/train_model.py (weights in
 *  tinyml_model.h). Inputs: temperature, RMS and peak vibration, current.
 *  Output: anomaly score 0..1 (0 = known normal operation).
 *
 *  - Plain C, no library: ≈ 600 multiplications per measurement
 *    (a few tens of µs on an ESP32), ≈ 1.5 KB of weights in flash.
 *  - Debounce: an anomaly is only raised after 3 consecutive measurements
 *    above the threshold, and cleared after 3 measurements well below it.
 *  - Explanation: find which quantity, when brought back to a normal value,
 *    lowers the score the most → "probable cause" of the anomaly.
 * ============================================================================
 */
#pragma once
#include <math.h>
#include <stdint.h>
#include "tinyml_model.h"

enum { TINYML_CAUSE_NONE = 0, TINYML_CAUSE_TEMP, TINYML_CAUSE_VIB, TINYML_CAUSE_CURRENT };
static const char *const TINYML_CAUSE_STR[] = {"", "temperature", "vibration", "current"};

typedef struct {
  float   score;     // latest score 0..1
  uint8_t anomaly;   // 1 = confirmed anomaly (after debounce)
  uint8_t cause;     // TINYML_CAUSE_*
  uint8_t above;     // debounce counters
  uint8_t below;
} tinyml_state_t;

/* Raw anomaly score of a measurement (1 - probability of "normal"). */
static inline float tinyml_score(float temp, float rms, float peak, float current) {
  float f[4] = {temp, current, log10f(rms + 0.01f), log10f(peak + 0.01f)};
  float x[4], h1[TINYML_H1], h2[TINYML_H2];
  for (int i = 0; i < 4; i++) {                              // scale to [-1, 1]
    float v = f[i] < TINYML_LO[i] ? TINYML_LO[i] : (f[i] > TINYML_HI[i] ? TINYML_HI[i] : f[i]);
    x[i] = (v - TINYML_LO[i]) / (TINYML_HI[i] - TINYML_LO[i]) * 2.0f - 1.0f;
  }
  for (int j = 0; j < TINYML_H1; j++) {                      // hidden layer 1
    float z = TINYML_B0[j];
    for (int i = 0; i < 4; i++) z += x[i] * TINYML_W0[i * TINYML_H1 + j];
    h1[j] = tanhf(z);
  }
  for (int j = 0; j < TINYML_H2; j++) {                      // hidden layer 2
    float z = TINYML_B1[j];
    for (int i = 0; i < TINYML_H1; i++) z += h1[i] * TINYML_W1[i * TINYML_H2 + j];
    h2[j] = tanhf(z);
  }
  float z = TINYML_B2[0];                                    // output (sigmoid)
  for (int i = 0; i < TINYML_H2; i++) z += h2[i] * TINYML_W2[i];
  return 1.0f - 1.0f / (1.0f + expf(-z));
}

/* Probable cause: the quantity which, brought back to a normal value, explains the anomaly. */
static inline uint8_t tinyml_explain(float temp, float rms, float peak, float current) {
  float best[4] = {1, 1, 1, 1};
  for (float t = 10; t <= 60; t += 5) {
    float s = tinyml_score(t, rms, peak, current);
    if (s < best[TINYML_CAUSE_TEMP]) best[TINYML_CAUSE_TEMP] = s;
  }
  static const float R[] = {0.0f, 0.03f, 0.06f, 0.1f, 0.15f};
  for (unsigned k = 0; k < sizeof R / sizeof R[0]; k++) {
    float s = tinyml_score(temp, R[k], R[k] * 1.6f, current);
    if (s < best[TINYML_CAUSE_VIB]) best[TINYML_CAUSE_VIB] = s;
  }
  for (float c = 0; c <= 4.0f; c += 0.5f) {
    float s = tinyml_score(temp, rms, peak, c);
    if (s < best[TINYML_CAUSE_CURRENT]) best[TINYML_CAUSE_CURRENT] = s;
  }
  // Priority temperature > vibration > current: pick the first quantity which,
  // corrected on its own, brings the machine back into the normal zone; else the best one.
  for (uint8_t k = TINYML_CAUSE_TEMP; k <= TINYML_CAUSE_CURRENT; k++)
    if (best[k] < TINYML_THRESHOLD * 0.6f) return k;
  uint8_t cause = TINYML_CAUSE_TEMP;
  for (uint8_t k = TINYML_CAUSE_VIB; k <= TINYML_CAUSE_CURRENT; k++)
    if (best[k] < best[cause]) cause = k;
  return cause;
}

/* Call on every measurement. Returns 1 if the anomaly state has just changed. */
static inline int tinyml_update(tinyml_state_t *st, float temp, float rms, float peak, float current) {
  st->score = tinyml_score(temp, rms, peak, current);
  uint8_t prev = st->anomaly;
  if (st->score > TINYML_THRESHOLD) {
    st->below = 0;
    if (st->above < 255) st->above++;
    if (st->above >= 3) st->anomaly = 1;
  } else if (st->score < TINYML_THRESHOLD * 0.6f) {
    st->above = 0;
    if (st->below < 255) st->below++;
    if (st->below >= 3) st->anomaly = 0;
  }
  if (st->anomaly && (!prev || st->cause == TINYML_CAUSE_NONE))
    st->cause = tinyml_explain(temp, rms, peak, current);
  if (!st->anomaly) st->cause = TINYML_CAUSE_NONE;
  return st->anomaly != prev;
}
