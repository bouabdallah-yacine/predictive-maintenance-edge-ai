// ============================================================================
//  Notifications Telegram des alertes importantes
//  Activé si TELEGRAM_TOKEN et TELEGRAM_CHAT_ID sont définis dans .env
//  Anti-spam : au plus 1 message par machine et par type d'alerte toutes les 60 s
// ============================================================================
const TOKEN   = process.env.TELEGRAM_TOKEN;
const CHAT_ID = process.env.TELEGRAM_CHAT_ID;
const COOLDOWN_MS = Number(process.env.TELEGRAM_COOLDOWN_SEC ?? 60) * 1000;

export const telegramEnabled = Boolean(TOKEN && CHAT_ID);
const lastSent = new Map();

const ICON = { CRITICAL: '🚨', WARNING: '⚠️', INFO: 'ℹ️' };

/** Faut-il notifier cette alerte ? (critiques, prédictives, perte de connexion) */
export function shouldNotify(alert) {
  if (alert.kind === 'PREDICTIVE') return true;
  return alert.severity === 'CRITICAL';
}

export function formatAlert(alert) {
  const time = new Date(alert.ts ?? Date.now()).toLocaleTimeString('fr-FR');
  return `${ICON[alert.severity] ?? '•'} <b>${alert.deviceId}</b> — ${alert.message}\n🕒 ${time}`;
}

export async function notify(alert, { fetchImpl = globalThis.fetch, now = Date.now() } = {}) {
  if (!telegramEnabled || !shouldNotify(alert)) return false;
  const key = `${alert.deviceId}|${alert.kind}|${alert.metric}`;
  if (now - (lastSent.get(key) ?? 0) < COOLDOWN_MS) return false;
  lastSent.set(key, now);
  try {
    const r = await fetchImpl(`https://api.telegram.org/bot${TOKEN}/sendMessage`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ chat_id: CHAT_ID, text: formatAlert(alert), parse_mode: 'HTML' }),
    });
    if (!r.ok) console.warn(`[TELEGRAM] refus ${r.status} : ${await r.text()}`);
    return r.ok;
  } catch (e) {
    console.warn(`[TELEGRAM] envoi impossible : ${e.message}`);
    return false;
  }
}
