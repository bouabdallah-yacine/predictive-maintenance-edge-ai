// ============================================================================
//  Telegram notifications for important alerts
//  Enabled when TELEGRAM_TOKEN and TELEGRAM_CHAT_ID are set in .env
//  Anti-spam: at most 1 message per machine and per alert type every 60 s
// ============================================================================
const TOKEN   = process.env.TELEGRAM_TOKEN;
const CHAT_ID = process.env.TELEGRAM_CHAT_ID;
const COOLDOWN_MS = Number(process.env.TELEGRAM_COOLDOWN_SEC ?? 60) * 1000;

export const telegramEnabled = Boolean(TOKEN && CHAT_ID);
const lastSent = new Map();

const ICON = { CRITICAL: '🚨', WARNING: '⚠️', INFO: 'ℹ️' };

/** Should this alert be notified? (critical, predictive, AI, connection loss) */
export function shouldNotify(alert) {
  if (alert.kind === 'PREDICTIVE') return true;
  if (alert.kind === 'AI' && alert.severity !== 'INFO') return true;   // edge AI anomaly
  return alert.severity === 'CRITICAL';
}

export function formatAlert(alert) {
  const time = new Date(alert.ts ?? Date.now()).toLocaleTimeString('en-GB');
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
    if (!r.ok) console.warn(`[TELEGRAM] rejected ${r.status}: ${await r.text()}`);
    return r.ok;
  } catch (e) {
    console.warn(`[TELEGRAM] could not send: ${e.message}`);
    return false;
  }
}

/**
 * Actually checks the configuration at startup by sending a test message.
 * Returns { ok, error } so a clear diagnostic can be displayed.
 */
export async function checkTelegram({ fetchImpl = globalThis.fetch } = {}) {
  if (!telegramEnabled) return { ok: false, error: 'TELEGRAM_TOKEN / TELEGRAM_CHAT_ID missing from .env' };
  try {
    const r = await fetchImpl(`https://api.telegram.org/bot${TOKEN}/sendMessage`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ chat_id: CHAT_ID, text: '🟢 Machine Monitor: server started, alerts active.' }),
    });
    if (r.ok) return { ok: true };
    const body = await r.json().catch(() => ({}));
    const why = r.status === 401 ? 'invalid token (TELEGRAM_TOKEN)'
      : r.status === 400 || r.status === 403 ? `wrong CHAT_ID or conversation not started (${body.description ?? r.status})`
      : `error ${r.status}`;
    return { ok: false, error: why };
  } catch (e) {
    return { ok: false, error: `Telegram unreachable (${e.message})` };
  }
}
