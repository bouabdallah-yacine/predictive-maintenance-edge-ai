import { test } from 'node:test';
import assert from 'node:assert/strict';
import { MachineAnalyzer, RollingStats, levelFor } from '../src/anomaly.js';

const normal = (i) => ({ temperature: 40 + Math.sin(i) * 0.5, vibRms: 0.05 + (i % 3) * 0.005, current: 1.5 + (i % 2) * 0.05 });

test('niveaux par seuil', () => {
  assert.equal(levelFor('temperature', 50), 'NORMAL');
  assert.equal(levelFor('temperature', 65), 'WARNING');
  assert.equal(levelFor('temperature', 80), 'CRITICAL');
  assert.equal(levelFor('current', 4.6), 'CRITICAL');
  assert.equal(levelFor('vibRms', undefined), 'NORMAL');
});

test('RollingStats : moyenne, écart-type, pente', () => {
  const s = new RollingStats(5);
  [1, 2, 3, 4, 5, 6].forEach((x) => s.push(x));
  assert.equal(s.n, 5);
  assert.equal(s.mean, 4);
  assert.ok(Math.abs(s.std - Math.sqrt(2.5)) < 1e-9);
  const t = new RollingStats(50);
  for (let i = 0; i < 20; i++) t.push(10 + 2 * i);
  assert.ok(Math.abs(t.slope() - 2) < 1e-9);
});

test('aucune alerte en fonctionnement normal', () => {
  const a = new MachineAnalyzer();
  let events = [];
  for (let i = 0; i < 120; i++) events = events.concat(a.analyze(normal(i)).events);
  assert.deepEqual(events, []);
});

test('une seule alerte par changement d\'état, puis retour à la normale', () => {
  const a = new MachineAnalyzer();
  for (let i = 0; i < 30; i++) a.analyze(normal(i));
  const e1 = a.analyze({ ...normal(30), vibRms: 0.7 }).events;
  assert.ok(e1.some((e) => e.kind === 'THRESHOLD' && e.metric === 'vibRms' && e.severity === 'CRITICAL'));
  const e2 = a.analyze({ ...normal(31), vibRms: 0.72 }).events;
  assert.equal(e2.filter((e) => e.metric === 'vibRms').length, 0, 'pas de doublon');
  const e3 = a.analyze(normal(32)).events;
  assert.ok(e3.some((e) => e.kind === 'RECOVERY' && e.metric === 'vibRms'));
});

test('z-score : détecte un saut sous le seuil', () => {
  const a = new MachineAnalyzer();
  for (let i = 0; i < 40; i++) a.analyze(normal(i));
  const r = a.analyze({ ...normal(40), vibRms: 0.2 });   // < 0,3 g mais x4
  assert.equal(r.levels.vibRms, 'NORMAL');
  assert.ok(r.events.some((e) => e.kind === 'STATISTICAL' && e.metric === 'vibRms'));
});

test('prédictif : montée de température → ETA avant surchauffe', () => {
  const a = new MachineAnalyzer();
  let pred = null, r;
  for (let i = 0; i < 60; i++) {
    r = a.analyze({ ...normal(i), temperature: 40 + i * 0.05 });   // +3 °C/min
    pred = pred ?? r.events.find((e) => e.kind === 'PREDICTIVE');
  }
  assert.ok(pred, 'alerte prédictive émise');
  assert.ok(r.etaCriticalMin > 5 && r.etaCriticalMin < 15, `eta=${r.etaCriticalMin}`);
});
