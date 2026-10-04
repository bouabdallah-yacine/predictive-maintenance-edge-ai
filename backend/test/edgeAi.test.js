import { test } from 'node:test';
import assert from 'node:assert/strict';
import { parseAi, aiEvent } from '../src/edgeAi.js';
import { shouldNotify } from '../src/notify.js';

test('parseAi : valide, borne et rejette les données invalides', () => {
  assert.deepEqual(parseAi({ score: 0.97, anomaly: true, cause: 'temperature' }),
    { score: 0.97, anomaly: true, cause: 'temperature' });
  assert.equal(parseAi({ score: 1.7 }).score, 1);
  assert.equal(parseAi(undefined), null);
  assert.equal(parseAi({ score: 'abc' }), null);
});

test('aiEvent : alerte seulement aux changements d\'état', () => {
  const anomaly = { score: 0.99, anomaly: true, cause: 'temperature' };
  const normal = { score: 0.01, anomaly: false, cause: '' };
  assert.equal(aiEvent(undefined, normal), null);           // démarrage normal
  const ev = aiEvent(false, anomaly);
  assert.equal(ev.kind, 'AI');
  assert.equal(ev.severity, 'WARNING');
  assert.match(ev.message, /température/);
  assert.equal(aiEvent(true, anomaly), null);               // déjà signalée
  assert.equal(aiEvent(true, normal).severity, 'INFO');     // retour à la normale
  assert.equal(shouldNotify(ev), true);                     // part sur Telegram
  assert.equal(shouldNotify(aiEvent(true, normal)), false);
});
