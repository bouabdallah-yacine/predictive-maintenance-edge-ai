// ============================================================================
//  Machine Monitor — Backend
//  MQTT (telemetry) → analysis → MongoDB → Socket.io (real time) + REST API
// ============================================================================
import 'dotenv/config';
import http from 'node:http';
import express from 'express';
import cors from 'cors';
import mqtt from 'mqtt';
import { Server as SocketServer } from 'socket.io';
import { createStore } from './store.js';
import { MachineAnalyzer, THRESHOLDS } from './anomaly.js';
import { notify, telegramEnabled, checkTelegram } from './notify.js';
import { parseAi, aiEvent } from './edgeAi.js';

const PORT         = Number(process.env.PORT ?? 4000);
const MQTT_URL     = process.env.MQTT_URL ?? 'mqtt://broker.hivemq.com:1883';
const TOPIC_PREFIX = process.env.TOPIC_PREFIX ?? 'pfe-monitor-7f3a';
// 127.0.0.1 rather than localhost: on Windows, Node tries IPv6 (::1) first
// while MongoDB only listens on IPv4 by default → ECONNREFUSED
const MONGO_URL    = process.env.MONGO_URL ?? 'mongodb://127.0.0.1:27017/machine_monitor';
const OFFLINE_SEC  = Number(process.env.OFFLINE_SEC ?? 10);

const store = await createStore(MONGO_URL);
const analyzers = new Map();          // deviceId → MachineAnalyzer
const devices = new Map();            // deviceId → { lastSeen, online, last }
const aiState = new Map();            // deviceId → last AI anomaly state (true/false)

const app = express();
app.use(cors());
app.use(express.json());
const server = http.createServer(app);
const io = new SocketServer(server, { cors: { origin: '*' } });

// ---------------------------------------------------------------------------
//  MQTT
// ---------------------------------------------------------------------------
// Diagnostics (without printing the password): what the backend actually read from .env
{
  const u = process.env.MQTT_USERNAME ?? '', p = process.env.MQTT_PASSWORD ?? '';
  const warn = [];
  if (/^\s|\s$/.test(u) || /^\s|\s$/.test(p)) warn.push('leading or trailing space');
  if (/["']/.test(p)) warn.push('quote character in the password');
  if (u && !p) warn.push('empty password (cut off by a #? wrap it in quotes)');
  console.log(`[MQTT] username "${u}" — password: ${p.length} characters${warn.length ? ` ⚠️ ${warn.join(', ')}` : ''}`);
}
const client = mqtt.connect(MQTT_URL, {
  clientId: `monitor-backend-${Math.random().toString(16).slice(2, 8)}`,
  reconnectPeriod: 2000,
  // Private broker: username / password (empty = public broker)
  username: process.env.MQTT_USERNAME || undefined,
  password: process.env.MQTT_PASSWORD || undefined,
});

client.on('connect', () => {
  console.log(`[MQTT] connected to ${MQTT_URL}${MQTT_URL.startsWith('mqtts') ? ' (TLS 🔒)' : ''}${process.env.MQTT_USERNAME ? ` as ${process.env.MQTT_USERNAME}` : ''}`);
  client.subscribe([`${TOPIC_PREFIX}/+/telemetry`, `${TOPIC_PREFIX}/+/status`], { qos: 0 });
  io.emit('broker', { connected: true });
});
client.on('reconnect', () => console.log('[MQTT] reconnecting...'));
client.on('close', () => io.emit('broker', { connected: false }));
client.on('error', (e) => console.error('[MQTT] error:', explainMqttError(e)));

// Human-readable error message (some network errors have an empty message)
function explainMqttError(e) {
  const inner = e.errors?.[0] ?? e;                 // AggregateError (IPv4 + IPv6)
  const code = inner.code ?? e.code ?? '';
  const msg = inner.message || e.message || String(code) || e.constructor?.name || 'unknown';
  const host = (() => { try { return new URL(MQTT_URL).hostname; } catch { return MQTT_URL; } })();
  const hints = {
    ENOTFOUND: `host "${host}" not found → check MQTT_URL in .env (no https://, no spaces)`,
    EAI_AGAIN: 'DNS resolution failed → check your Internet connection',
    ECONNREFUSED: 'connection refused → check the port (8883 for mqtts)',
    ETIMEDOUT: 'timed out → port 8883 may be blocked by your network (try tethering through your phone)',
    ECONNRESET: 'connection reset → check that the URL starts with mqtts:// and that the port is 8883',
  };
  if (/Not authorized|Bad User Name|bad username/i.test(msg)) return `${msg} → check MQTT_USERNAME / MQTT_PASSWORD in .env`;
  if (/certificate|self.signed|unable to verify/i.test(msg)) return `${msg} → TLS certificate problem`;
  return `${msg}${code && !msg.includes(code) ? ` (${code})` : ''}${hints[code] ? ` → ${hints[code]}` : ''}`;
}

client.on('message', async (topic, payload) => {
  const [, deviceId, kind] = topic.split('/');
  try {
    if (kind === 'status') return handleStatus(deviceId, payload.toString());
    if (kind === 'telemetry') return await handleTelemetry(deviceId, JSON.parse(payload.toString()));
  } catch (e) {
    console.warn(`[MQTT] message ignored on ${topic}: ${e.message}`);
  }
});

function num(v) {
  const n = Number(v);
  return Number.isFinite(n) ? n : undefined;
}

async function handleTelemetry(deviceId, msg) {
  const sample = {
    deviceId,
    ts: new Date(),
    temperature: num(msg.temperature),
    humidity:    num(msg.humidity),
    vibRms:      num(msg.vibRms),
    vibPeak:     num(msg.vibPeak),
    current:     num(msg.current),
    deviceState: msg.state,
    faultInjected: Boolean(msg.faultInjected),
  };
  const ai = parseAi(msg.ai);               // edge AI verdict (ESP32)
  if (ai) Object.assign(sample, { aiScore: ai.score, aiAnomaly: ai.anomaly, aiCause: ai.cause });

  if (!analyzers.has(deviceId)) analyzers.set(deviceId, new MachineAnalyzer());
  const result = analyzers.get(deviceId).analyze(sample);
  sample.state = result.state;
  sample.zscores = result.zscores;

  const saved = await store.saveTelemetry(sample);
  markSeen(deviceId, saved);

  io.emit('telemetry', { ...saved, levels: result.levels, etaCriticalMin: result.etaCriticalMin });

  const events = [...result.events];
  const aiEv = aiEvent(aiState.get(deviceId), ai);
  if (ai) aiState.set(deviceId, ai.anomaly);
  if (aiEv) events.push(aiEv);

  for (const ev of events) {
    const alert = await store.saveAlert({ deviceId, ts: new Date(), ...ev });
    io.emit('alert', alert);
    notify(alert);
    console.log(`[ALERT] ${deviceId} ${ev.severity} — ${ev.message}`);
  }
}

function markSeen(deviceId, last) {
  const d = devices.get(deviceId) ?? { deviceId, online: false };
  const wasOnline = d.online;
  Object.assign(d, { lastSeen: Date.now(), online: true, last });
  devices.set(deviceId, d);
  if (!wasOnline) io.emit('device', publicDevice(d));
}

async function handleStatus(deviceId, status) {
  const d = devices.get(deviceId) ?? { deviceId, online: false };
  const online = status === 'online';
  if (d.online === online) return;
  d.online = online;
  if (online) d.lastSeen = Date.now();   // the watchdog starts from the connection time
  devices.set(deviceId, d);
  io.emit('device', publicDevice(d));
  const alert = await store.saveAlert({
    deviceId, ts: new Date(), kind: 'CONNECTIVITY', metric: 'link',
    severity: online ? 'INFO' : 'CRITICAL',
    message: online ? 'Device connected' : 'Device offline (MQTT Last Will)',
  });
  io.emit('alert', alert);
  notify(alert);
}

// Server-side watchdog: no data for OFFLINE_SEC → offline
setInterval(async () => {
  for (const d of devices.values()) {
    if (d.online && Date.now() - (d.lastSeen ?? 0) > OFFLINE_SEC * 1000) {
      await handleStatus(d.deviceId, 'offline');
    }
  }
}, 2000);

const publicDevice = (d) => ({ deviceId: d.deviceId, online: d.online, lastSeen: d.lastSeen, last: d.last });

// ---------------------------------------------------------------------------
//  API REST
// ---------------------------------------------------------------------------
app.get('/api/health', (_req, res) => {
  res.json({ ok: true, mqtt: client.connected, store: store.name, telegram: telegramEnabled, topicPrefix: TOPIC_PREFIX });
});

app.get('/api/config', (_req, res) => res.json({ thresholds: THRESHOLDS }));

app.get('/api/devices', (_req, res) => res.json([...devices.values()].map(publicDevice)));

app.get('/api/telemetry', async (req, res) => {
  const minutes = Math.min(Number(req.query.minutes ?? 10), 7 * 24 * 60);
  const since = new Date(Date.now() - minutes * 60_000);
  res.json(await store.queryTelemetry({ deviceId: req.query.deviceId, since }));
});

app.get('/api/stats/:deviceId', async (req, res) => {
  const minutes = Number(req.query.minutes ?? 60);
  res.json(await store.stats(req.params.deviceId, new Date(Date.now() - minutes * 60_000)));
});

app.get('/api/alerts', async (req, res) => {
  res.json(await store.listAlerts({ limit: Number(req.query.limit ?? 100), deviceId: req.query.deviceId }));
});

app.post('/api/alerts/:id/ack', async (req, res) => {
  const a = await store.ackAlert(req.params.id);
  if (!a) return res.status(404).json({ error: 'alert not found' });
  io.emit('alert-updated', a);
  res.json(a);
});

// Downlink command to the device (dashboard → MQTT → ESP32)
const ALLOWED_CMDS = new Set(['fault_on', 'fault_off', 'mute', 'unmute']);
app.post('/api/devices/:id/cmd', (req, res) => {
  const { cmd } = req.body ?? {};
  if (!ALLOWED_CMDS.has(cmd)) return res.status(400).json({ error: `invalid command (${[...ALLOWED_CMDS].join(', ')})` });
  client.publish(`${TOPIC_PREFIX}/${req.params.id}/cmd`, cmd);
  res.json({ sent: cmd });
});

// CSV export of the history
app.get('/api/export.csv', async (req, res) => {
  const minutes = Number(req.query.minutes ?? 60);
  const rows = await store.queryTelemetry({
    deviceId: req.query.deviceId, since: new Date(Date.now() - minutes * 60_000), limit: 100000,
  });
  const cols = ['ts', 'deviceId', 'temperature', 'humidity', 'vibRms', 'vibPeak', 'current', 'state', 'aiScore'];
  const csv = [cols.join(';')]
    .concat(rows.map((r) => cols.map((c) => (c === 'ts' ? new Date(r.ts).toISOString() : r[c] ?? '')).join(';')))
    .join('\n');
  res.setHeader('Content-Type', 'text/csv; charset=utf-8');
  res.setHeader('Content-Disposition', 'attachment; filename="telemetry.csv"');
  res.send(csv);
});

io.on('connection', (socket) => {
  socket.emit('broker', { connected: client.connected });
  socket.emit('devices', [...devices.values()].map(publicDevice));
});

server.listen(PORT, () => {
  console.log(`[HTTP] API + Socket.io on http://localhost:${PORT}`);
  checkTelegram().then(({ ok, error }) => {
    console.log(ok ? '[TELEGRAM] ✅ notifications enabled (test message sent to your phone)'
                   : `[TELEGRAM] ❌ ${error}`);
  });
});
