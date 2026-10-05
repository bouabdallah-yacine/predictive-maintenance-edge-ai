import { test } from 'node:test';
import assert from 'node:assert/strict';
import { parseAi, aiEvent, causeLabel } from '../src/edgeAi.js';
import { shouldNotify } from '../src/notify.js';

test('parseAi: validates, clamps and rejects invalid data', () => {
  assert.deepEqual(parseAi({ score: 0.97, anomaly: true, cause: 'temperature' }),
    { score: 0.97, anomaly: true, cause: 'temperature' });
  assert.equal(parseAi({ score: 1.7 }).score, 1);
  assert.equal(parseAi(undefined), null);
  assert.equal(parseAi({ score: 'abc' }), null);
});

test('aiEvent: alerts only on state changes', () => {
  const anomaly = { score: 0.99, anomaly: true, cause: 'temperature' };
  const normal = { score: 0.01, anomaly: false, cause: '' };
  assert.equal(aiEvent(undefined, normal), null);           // normal startup
  const ev = aiEvent(false, anomaly);
  assert.equal(ev.kind, 'AI');
  assert.equal(ev.severity, 'WARNING');
  assert.match(ev.message, /probable cause: temperature/);
  assert.equal(aiEvent(true, anomaly), null);               // already reported
  assert.equal(aiEvent(true, normal).severity, 'INFO');     // back to normal
  assert.equal(shouldNotify(ev), true);                     // sent to Telegram
  assert.equal(shouldNotify(aiEvent(true, normal)), false);
});

test('causeLabel: French and English cause values are displayed in English', () => {
  assert.equal(causeLabel('température'), 'temperature');
  assert.equal(causeLabel('temperature'), 'temperature');
  assert.equal(causeLabel('courant'), 'current');
  assert.equal(causeLabel('current'), 'current');
  assert.equal(causeLabel('humidité'), 'humidity');
  assert.equal(causeLabel('vibration'), 'vibration');
  assert.equal(causeLabel(''), '');
  const ev = aiEvent(false, { score: 0.9, anomaly: true, cause: 'courant' });
  assert.match(ev.message, /probable cause: current/);
  assert.equal(ev.metric, 'courant');                      // raw value kept as received
});
