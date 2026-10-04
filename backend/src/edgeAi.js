// ============================================================================
//  IA embarquée : traitement du verdict calculé PAR L'ESP32 (champ "ai")
//  Le backend ne recalcule rien : il crée une alerte quand l'état change.
// ============================================================================
const CAUSES = { temperature: 'température', vibration: 'vibration', courant: 'courant' };

/** Normalise le champ "ai" reçu en MQTT (ou null si absent / invalide). */
export function parseAi(ai) {
  if (!ai || typeof ai !== 'object') return null;
  const score = Number(ai.score);
  if (!Number.isFinite(score)) return null;
  return { score: Math.min(Math.max(score, 0), 1), anomaly: Boolean(ai.anomaly), cause: String(ai.cause ?? '') };
}

/**
 * Compare au verdict précédent et renvoie l'événement d'alerte éventuel.
 * @param {boolean|undefined} wasAnomaly  état précédent pour cette machine
 * @param {{score:number, anomaly:boolean, cause:string}} ai
 */
export function aiEvent(wasAnomaly, ai) {
  if (!ai || Boolean(wasAnomaly) === ai.anomaly) return null;
  if (ai.anomaly) {
    const cause = CAUSES[ai.cause] ?? ai.cause;
    return {
      kind: 'AI', metric: ai.cause || 'global', severity: 'WARNING', value: ai.score,
      message: `🤖 IA embarquée : comportement anormal détecté${cause ? ` (cause probable : ${cause})` : ''} — score ${Math.round(ai.score * 100)} %`,
    };
  }
  if (wasAnomaly === undefined) return null;           // premier message : rien à signaler
  return { kind: 'AI', metric: 'global', severity: 'INFO', value: ai.score, message: '🤖 IA embarquée : retour au fonctionnement normal' };
}
