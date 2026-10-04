/*
 * ============================================================================
 *  TinyML — détection d'anomalies embarquée (inférence du réseau de neurones)
 * ============================================================================
 *  Réseau 4 → 16 → 16 → 1 entraîné par ai/train_model.py (poids dans
 *  tinyml_model.h). Entrées : température, vibration RMS et crête, courant.
 *  Sortie : score d'anomalie 0..1 (0 = fonctionnement normal connu).
 *
 *  - Tout en C pur, sans bibliothèque : ≈ 600 multiplications par mesure
 *    (quelques dizaines de µs sur ESP32), ≈ 1,5 Ko de poids en flash.
 *  - Anti-rebond : l'anomalie n'est déclarée qu'après 3 mesures consécutives
 *    au-dessus du seuil, et levée après 3 mesures nettement en dessous.
 *  - Explication : on cherche quelle grandeur, ramenée à une valeur normale,
 *    fait le plus baisser le score → « cause probable » de l'anomalie.
 * ============================================================================
 */
#pragma once
#include <math.h>
#include <stdint.h>
#include "tinyml_model.h"

enum { TINYML_CAUSE_NONE = 0, TINYML_CAUSE_TEMP, TINYML_CAUSE_VIB, TINYML_CAUSE_CURRENT };
static const char *const TINYML_CAUSE_STR[] = {"", "temperature", "vibration", "courant"};

typedef struct {
  float   score;     // dernier score 0..1
  uint8_t anomaly;   // 1 = anomalie confirmée (après anti-rebond)
  uint8_t cause;     // TINYML_CAUSE_*
  uint8_t above;     // compteurs d'anti-rebond
  uint8_t below;
} tinyml_state_t;

/* Score d'anomalie brut d'une mesure (1 - probabilité « normal »). */
static inline float tinyml_score(float temp, float rms, float peak, float current) {
  float f[4] = {temp, current, log10f(rms + 0.01f), log10f(peak + 0.01f)};
  float x[4], h1[TINYML_H1], h2[TINYML_H2];
  for (int i = 0; i < 4; i++) {                              // mise à l'échelle [-1, 1]
    float v = f[i] < TINYML_LO[i] ? TINYML_LO[i] : (f[i] > TINYML_HI[i] ? TINYML_HI[i] : f[i]);
    x[i] = (v - TINYML_LO[i]) / (TINYML_HI[i] - TINYML_LO[i]) * 2.0f - 1.0f;
  }
  for (int j = 0; j < TINYML_H1; j++) {                      // couche cachée 1
    float z = TINYML_B0[j];
    for (int i = 0; i < 4; i++) z += x[i] * TINYML_W0[i * TINYML_H1 + j];
    h1[j] = tanhf(z);
  }
  for (int j = 0; j < TINYML_H2; j++) {                      // couche cachée 2
    float z = TINYML_B1[j];
    for (int i = 0; i < TINYML_H1; i++) z += h1[i] * TINYML_W1[i * TINYML_H2 + j];
    h2[j] = tanhf(z);
  }
  float z = TINYML_B2[0];                                    // sortie (sigmoïde)
  for (int i = 0; i < TINYML_H2; i++) z += h2[i] * TINYML_W2[i];
  return 1.0f - 1.0f / (1.0f + expf(-z));
}

/* Cause probable : la grandeur qui, ramenée à une valeur normale, explique l'anomalie. */
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
  // Priorité température > vibration > courant : on retient la première grandeur
  // qui, corrigée seule, ramène la machine dans la zone normale ; sinon la meilleure.
  for (uint8_t k = TINYML_CAUSE_TEMP; k <= TINYML_CAUSE_CURRENT; k++)
    if (best[k] < TINYML_THRESHOLD * 0.6f) return k;
  uint8_t cause = TINYML_CAUSE_TEMP;
  for (uint8_t k = TINYML_CAUSE_VIB; k <= TINYML_CAUSE_CURRENT; k++)
    if (best[k] < best[cause]) cause = k;
  return cause;
}

/* À appeler à chaque mesure. Renvoie 1 si l'état d'anomalie vient de changer. */
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
