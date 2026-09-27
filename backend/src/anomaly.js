// ============================================================================
//  Détection d'anomalies côté serveur
//  1) Règles métier (seuils) → reprises du firmware, pour ne pas dépendre
//     uniquement de l'équipement.
//  2) Statistique : z-score sur fenêtre glissante. Détecte un comportement
//     "inhabituel" même sous les seuils (ex. vibration qui double
//     soudainement mais reste < 0,3 g).
//  3) Tendance : pente de la température (régression linéaire) → alerte
//     prédictive "surchauffe probable dans N minutes".
// ============================================================================

export const THRESHOLDS = {
  temperature: { warn: 60, crit: 75, unit: '°C' },
  vibRms:      { warn: 0.30, crit: 0.60, unit: 'g' },
  current:     { warn: 3.5, crit: 4.5, unit: 'A' },
};

// Libellés des alertes (ceux du cahier des charges)
export const ALERT_TYPES = {
  temperature: { WARNING: 'Température élevée', CRITICAL: 'Surchauffe' },
  vibRms:      { WARNING: 'Vibration anormale', CRITICAL: 'Anomalie de vibration' },
  current:     { WARNING: 'Courant anormal', CRITICAL: 'Défaillance possible (surintensité)' },
};

export function levelFor(metric, value) {
  const t = THRESHOLDS[metric];
  if (!t || typeof value !== 'number' || Number.isNaN(value)) return 'NORMAL';
  if (value >= t.crit) return 'CRITICAL';
  if (value >= t.warn) return 'WARNING';
  return 'NORMAL';
}

/** Fenêtre glissante avec moyenne / écart-type en O(1). */
export class RollingStats {
  constructor(size = 60) {
    this.size = size;
    this.buf = [];
    this.sum = 0;
    this.sumSq = 0;
  }
  push(x) {
    this.buf.push(x);
    this.sum += x;
    this.sumSq += x * x;
    if (this.buf.length > this.size) {
      const old = this.buf.shift();
      this.sum -= old;
      this.sumSq -= old * old;
    }
  }
  get n() { return this.buf.length; }
  get mean() { return this.n ? this.sum / this.n : 0; }
  get std() {
    if (this.n < 2) return 0;
    const v = (this.sumSq - (this.sum * this.sum) / this.n) / (this.n - 1);
    return Math.sqrt(Math.max(v, 0));
  }
  /** Pente (unité / échantillon) par moindres carrés. */
  slope() {
    const n = this.n;
    if (n < 10) return 0;
    const mx = (n - 1) / 2;
    let num = 0, den = 0;
    for (let i = 0; i < n; i++) {
      num += (i - mx) * (this.buf[i] - this.mean);
      den += (i - mx) ** 2;
    }
    return num / den;
  }
}

/**
 * Un détecteur par machine. analyze() renvoie les niveaux calculés et la
 * liste des *nouveaux* événements (changement d'état uniquement, pour ne pas
 * inonder la base d'alertes identiques chaque seconde).
 */
export class MachineAnalyzer {
  constructor({ window = 60, zThreshold = 3.5, minStd = { temperature: 0.3, vibRms: 0.02, current: 0.05 }, sampleSec = 1 } = {}) {
    this.zThreshold = zThreshold;
    this.minStd = minStd;
    this.sampleSec = sampleSec;
    this.stats = {};
    this.prevLevel = {};
    this.prevZ = {};
    this.prevTrend = false;
    for (const m of Object.keys(THRESHOLDS)) {
      this.stats[m] = new RollingStats(window);
      this.prevLevel[m] = 'NORMAL';
      this.prevZ[m] = false;
    }
  }

  analyze(sample) {
    const events = [];
    const levels = {};
    const zscores = {};

    for (const m of Object.keys(THRESHOLDS)) {
      const v = sample[m];
      if (typeof v !== 'number' || Number.isNaN(v)) continue;
      const st = this.stats[m];

      // z-score calculé AVANT d'ajouter la valeur (on la compare au passé)
      const std = Math.max(st.std, this.minStd[m] ?? 0);
      const z = st.n >= 20 ? (v - st.mean) / std : 0;
      zscores[m] = Math.round(z * 100) / 100;

      const lvl = levelFor(m, v);
      levels[m] = lvl;

      if (lvl !== this.prevLevel[m]) {
        if (lvl !== 'NORMAL') {
          events.push({
            kind: 'THRESHOLD', metric: m, severity: lvl, value: v,
            message: `${ALERT_TYPES[m][lvl]} : ${v} ${THRESHOLDS[m].unit}`,
          });
        } else {
          events.push({
            kind: 'RECOVERY', metric: m, severity: 'INFO', value: v,
            message: `Retour à la normale (${m}) : ${v} ${THRESHOLDS[m].unit}`,
          });
        }
        this.prevLevel[m] = lvl;
      }

      const isOutlier = Math.abs(z) >= this.zThreshold;
      if (isOutlier && !this.prevZ[m] && lvl === 'NORMAL') {
        events.push({
          kind: 'STATISTICAL', metric: m, severity: 'WARNING', value: v, zscore: zscores[m],
          message: `Comportement inhabituel (${m}) : z = ${zscores[m]} (moy. ${st.mean.toFixed(2)})`,
        });
      }
      this.prevZ[m] = isOutlier;

      // On n'intègre pas les valeurs aberrantes à la référence "normale"
      if (!isOutlier || st.n < 20) st.push(v);
    }

    // Maintenance prédictive : temps estimé avant le seuil critique
    let etaCriticalMin = null;
    const ts = this.stats.temperature;
    const slopePerSec = ts.slope() / this.sampleSec;
    if (slopePerSec > 0.005 && typeof sample.temperature === 'number' && sample.temperature < THRESHOLDS.temperature.crit) {
      etaCriticalMin = Math.round(((THRESHOLDS.temperature.crit - sample.temperature) / slopePerSec) / 60 * 10) / 10;
      const trending = etaCriticalMin < 15;
      if (trending && !this.prevTrend) {
        events.push({
          kind: 'PREDICTIVE', metric: 'temperature', severity: 'WARNING', value: sample.temperature,
          message: `Surchauffe prévue dans ~${etaCriticalMin} min (+${(slopePerSec * 60).toFixed(2)} °C/min)`,
        });
      }
      this.prevTrend = trending;
    } else {
      this.prevTrend = false;
    }

    const order = { NORMAL: 0, WARNING: 1, CRITICAL: 2 };
    const state = Object.values(levels).reduce((a, b) => (order[b] > order[a] ? b : a), 'NORMAL');
    return { levels, zscores, state, etaCriticalMin, events };
  }
}
