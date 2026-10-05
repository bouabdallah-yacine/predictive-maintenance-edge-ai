import { test } from 'node:test';
import assert from 'node:assert/strict';

process.env.TELEGRAM_TOKEN = 'TEST';
process.env.TELEGRAM_CHAT_ID = '42';
const { notify, shouldNotify, formatAlert } = await import('../src/notify.js');

const crit = { deviceId: 'stm32-01', kind: 'THRESHOLD', metric: 'temperature', severity: 'CRITICAL', message: 'Overheating: 80 °C', ts: Date.now() };

test('filter: critical and predictive only', () => {
  assert.equal(shouldNotify(crit), true);
  assert.equal(shouldNotify({ ...crit, severity: 'WARNING' }), false);
  assert.equal(shouldNotify({ ...crit, severity: 'WARNING', kind: 'PREDICTIVE' }), true);
  assert.equal(shouldNotify({ ...crit, severity: 'INFO', kind: 'RECOVERY' }), false);
});

test('sending + 60 s anti-spam', async () => {
  const calls = [];
  const fetchImpl = async (url, opts) => { calls.push({ url, body: JSON.parse(opts.body) }); return { ok: true }; };
  assert.equal(await notify(crit, { fetchImpl, now: 1_000_000 }), true);
  assert.equal(await notify(crit, { fetchImpl, now: 1_030_000 }), false);   // < 60 s
  assert.equal(await notify(crit, { fetchImpl, now: 1_061_000 }), true);
  assert.equal(calls.length, 2);
  assert.match(calls[0].url, /botTEST\/sendMessage$/);
  assert.equal(calls[0].body.chat_id, '42');
  assert.match(formatAlert(crit), /stm32-01.*Overheating/);
});

test('checkTelegram: clear diagnostics', async () => {
  const { checkTelegram } = await import('../src/notify.js');
  const ok = await checkTelegram({ fetchImpl: async () => ({ ok: true }) });
  assert.equal(ok.ok, true);
  const bad = await checkTelegram({ fetchImpl: async () => ({ ok: false, status: 400, json: async () => ({ description: 'Bad Request: chat not found' }) }) });
  assert.equal(bad.ok, false);
  assert.match(bad.error, /wrong CHAT_ID.*chat not found/);
  const tok = await checkTelegram({ fetchImpl: async () => ({ ok: false, status: 401, json: async () => ({}) }) });
  assert.match(tok.error, /invalid token/);
});
