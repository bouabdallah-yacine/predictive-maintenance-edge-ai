import { test } from 'node:test';
import assert from 'node:assert/strict';
import { FrameParser, TelnetFilter } from '../src/frameParser.js';

const feedAll = (p, s) => [...s].map((c) => p.feed(c)).filter(Boolean);

test('trame produite par le code C du STM32', () => {
  // Trame générée par proto_encode() (test_protocol.c)
  const [f] = feedAll(new FrameParser(), '$MM,42,253,451,85,140,1620,3*20\r\n');
  assert.equal(f.seq, 42);
  assert.equal(f.temperature, 25.3);
  assert.equal(f.vibRms, 0.085);
  assert.equal(f.current, 1.62);
  assert.equal(f.state, 'NORMAL');
  assert.deepEqual(f.health, { dht: true, mpu: true });
});

test('niveau et panne dans les flags', () => {
  const body = 'MM,1,800,450,700,900,5000,38';          // 38 = 0b100110 : critique + panne + mpu
  let x = 0; for (const c of body) x ^= c.charCodeAt(0);
  const [f] = feedAll(new FrameParser(), `$${body}*${x.toString(16).toUpperCase().padStart(2, '0')}\r\n`);
  assert.equal(f.state, 'CRITICAL');
  assert.equal(f.faultInjected, true);
});

test('checksum faux et texte parasite ignorés', () => {
  const p = new FrameParser();
  assert.equal(feedAll(p, '# Machine Monitor\r\n$MM,42,253,451,85,140,1620,3*21\r\n').length, 0);
  assert.equal(p.stats.errChecksum, 1);
  assert.equal(feedAll(p, 'bruit$MM,1,2$MM,42,253,451,85,140,1620,3*20').length, 1);
});

test('filtre Telnet (RFC2217) : retire la négociation', () => {
  const t = new TelnetFilter();
  const neg = Buffer.from([255, 251, 1, 255, 253, 44, 255, 250, 44, 1, 0, 0, 255, 240]);
  const data = Buffer.from('$MM,42,253,451,85,140,1620,3*20\r\n');
  const out = t.push(Buffer.concat([neg, data.subarray(0, 10)])) + t.push(data.subarray(10));
  assert.equal(out, data.toString());
});

test('filtre Telnet : répond à la négociation (accepte BINARY/SGA, refuse le reste)', () => {
  const t = new TelnetFilter();
  t.push(Buffer.from([255, 251, 0, 255, 251, 3, 255, 253, 44, 255, 251, 1]));
  assert.deepEqual([...t.takeReplies()], [255, 253, 0, 255, 253, 3, 255, 252, 44, 255, 254, 1]);
  assert.equal(t.takeReplies().length, 0);
});
