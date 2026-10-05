// ============================================================================
//  Edge AI: handling of the verdict computed BY THE ESP32 ("ai" field)
//  The backend does not recompute anything: it raises an alert when the state changes.
// ============================================================================

// The firmware may report the probable cause in French ("température",
// "courant", "humidité", ...). Both the French and English values are accepted;
// they are always displayed in English.
const CAUSES = {
  temperature: 'temperature', 'température': 'temperature',
  vibration: 'vibration',
  current: 'current', courant: 'current',
  humidity: 'humidity', 'humidité': 'humidity', humidite: 'humidity',
};

/** English label for an AI cause value (unknown values are returned as-is). */
export function causeLabel(cause) {
  if (!cause) return '';
  const key = String(cause).trim().toLowerCase();
  return CAUSES[key] ?? String(cause);
}

/** Normalises the "ai" field received over MQTT (or null if missing / invalid). */
export function parseAi(ai) {
  if (!ai || typeof ai !== 'object') return null;
  const score = Number(ai.score);
  if (!Number.isFinite(score)) return null;
  return { score: Math.min(Math.max(score, 0), 1), anomaly: Boolean(ai.anomaly), cause: String(ai.cause ?? '') };
}

/**
 * Compares with the previous verdict and returns the alert event, if any.
 * @param {boolean|undefined} wasAnomaly  previous state for this machine
 * @param {{score:number, anomaly:boolean, cause:string}} ai
 */
export function aiEvent(wasAnomaly, ai) {
  if (!ai || Boolean(wasAnomaly) === ai.anomaly) return null;
  if (ai.anomaly) {
    const cause = causeLabel(ai.cause);
    return {
      kind: 'AI', metric: ai.cause || 'global', severity: 'WARNING', value: ai.score,
      message: `🤖 Edge AI: abnormal behaviour detected${cause ? ` (probable cause: ${cause})` : ''} — score ${Math.round(ai.score * 100)}%`,
    };
  }
  if (wasAnomaly === undefined) return null;           // first message: nothing to report
  return { kind: 'AI', metric: 'global', severity: 'INFO', value: ai.score, message: '🤖 Edge AI: back to normal operation' };
}
