import { test } from 'node:test';
import assert from 'node:assert/strict';

process.env.TELEGRAM_TOKEN = 'TEST';
process.env.TELEGRAM_CHAT_ID = '42';
const { notify, shouldNotify, formatAlert } = await import('../src/notify.js');

const crit = { deviceId: 'stm32-01', kind: 'THRESHOLD', metric: 'temperature', severity: 'CRITICAL', message: 'Surchauffe : 80 °C', ts: Date.now() };

test('filtre : critiques et prédictives seulement', () => {
  assert.equal(shouldNotify(crit), true);
  assert.equal(shouldNotify({ ...crit, severity: 'WARNING' }), false);
  assert.equal(shouldNotify({ ...crit, severity: 'WARNING', kind: 'PREDICTIVE' }), true);
  assert.equal(shouldNotify({ ...crit, severity: 'INFO', kind: 'RECOVERY' }), false);
});

test('envoi + anti-spam 60 s', async () => {
  const calls = [];
  const fetchImpl = async (url, opts) => { calls.push({ url, body: JSON.parse(opts.body) }); return { ok: true }; };
  assert.equal(await notify(crit, { fetchImpl, now: 1_000_000 }), true);
  assert.equal(await notify(crit, { fetchImpl, now: 1_030_000 }), false);   // < 60 s
  assert.equal(await notify(crit, { fetchImpl, now: 1_061_000 }), true);
  assert.equal(calls.length, 2);
  assert.match(calls[0].url, /botTEST\/sendMessage$/);
  assert.equal(calls[0].body.chat_id, '42');
  assert.match(formatAlert(crit), /stm32-01.*Surchauffe/);
});
