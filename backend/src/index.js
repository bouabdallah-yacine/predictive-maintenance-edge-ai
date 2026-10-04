// ============================================================================
//  Machine Monitor — Backend
//  MQTT (télémétrie) → analyse → MongoDB → Socket.io (temps réel) + API REST
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

const PORT         = Number(process.env.PORT ?? 4000);
const MQTT_URL     = process.env.MQTT_URL ?? 'mqtt://broker.hivemq.com:1883';
const TOPIC_PREFIX = process.env.TOPIC_PREFIX ?? 'pfe-monitor-7f3a';
// 127.0.0.1 plutôt que localhost : sous Windows, Node essaie d'abord l'IPv6 (::1)
// alors que MongoDB n'écoute qu'en IPv4 par défaut → ECONNREFUSED
const MONGO_URL    = process.env.MONGO_URL ?? 'mongodb://127.0.0.1:27017/machine_monitor';
const OFFLINE_SEC  = Number(process.env.OFFLINE_SEC ?? 10);

const store = await createStore(MONGO_URL);
const analyzers = new Map();          // deviceId → MachineAnalyzer
const devices = new Map();            // deviceId → { lastSeen, online, last }

const app = express();
app.use(cors());
app.use(express.json());
const server = http.createServer(app);
const io = new SocketServer(server, { cors: { origin: '*' } });

// ---------------------------------------------------------------------------
//  MQTT
// ---------------------------------------------------------------------------
const client = mqtt.connect(MQTT_URL, {
  clientId: `monitor-backend-${Math.random().toString(16).slice(2, 8)}`,
  reconnectPeriod: 2000,
  // Broker privé : identifiant / mot de passe (vides = broker public)
  username: process.env.MQTT_USERNAME || undefined,
  password: process.env.MQTT_PASSWORD || undefined,
});

client.on('connect', () => {
  console.log(`[MQTT] connecté à ${MQTT_URL}${MQTT_URL.startsWith('mqtts') ? ' (TLS 🔒)' : ''}${process.env.MQTT_USERNAME ? ` en tant que ${process.env.MQTT_USERNAME}` : ''}`);
  client.subscribe([`${TOPIC_PREFIX}/+/telemetry`, `${TOPIC_PREFIX}/+/status`], { qos: 0 });
  io.emit('broker', { connected: true });
});
client.on('reconnect', () => console.log('[MQTT] reconnexion...'));
client.on('close', () => io.emit('broker', { connected: false }));
client.on('error', (e) => console.error('[MQTT] erreur :', e.message, /Not authorized|Bad User Name/i.test(e.message) ? '→ vérifie MQTT_USERNAME / MQTT_PASSWORD dans .env' : ''));

client.on('message', async (topic, payload) => {
  const [, deviceId, kind] = topic.split('/');
  try {
    if (kind === 'status') return handleStatus(deviceId, payload.toString());
    if (kind === 'telemetry') return await handleTelemetry(deviceId, JSON.parse(payload.toString()));
  } catch (e) {
    console.warn(`[MQTT] message ignoré sur ${topic} : ${e.message}`);
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

  if (!analyzers.has(deviceId)) analyzers.set(deviceId, new MachineAnalyzer());
  const result = analyzers.get(deviceId).analyze(sample);
  sample.state = result.state;
  sample.zscores = result.zscores;

  const saved = await store.saveTelemetry(sample);
  markSeen(deviceId, saved);

  io.emit('telemetry', { ...saved, levels: result.levels, etaCriticalMin: result.etaCriticalMin });

  for (const ev of result.events) {
    const alert = await store.saveAlert({ deviceId, ts: new Date(), ...ev });
    io.emit('alert', alert);
    notify(alert);
    console.log(`[ALERTE] ${deviceId} ${ev.severity} — ${ev.message}`);
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
  if (online) d.lastSeen = Date.now();   // le watchdog part de la connexion
  devices.set(deviceId, d);
  io.emit('device', publicDevice(d));
  const alert = await store.saveAlert({
    deviceId, ts: new Date(), kind: 'CONNECTIVITY', metric: 'link',
    severity: online ? 'INFO' : 'CRITICAL',
    message: online ? 'Équipement connecté' : 'Équipement hors ligne (Last Will MQTT)',
  });
  io.emit('alert', alert);
  notify(alert);
}

// Watchdog côté serveur : plus de données depuis OFFLINE_SEC → hors ligne
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
  if (!a) return res.status(404).json({ error: 'alerte introuvable' });
  io.emit('alert-updated', a);
  res.json(a);
});

// Commande descendante vers l'équipement (dashboard → MQTT → ESP32)
const ALLOWED_CMDS = new Set(['fault_on', 'fault_off', 'mute', 'unmute']);
app.post('/api/devices/:id/cmd', (req, res) => {
  const { cmd } = req.body ?? {};
  if (!ALLOWED_CMDS.has(cmd)) return res.status(400).json({ error: `commande invalide (${[...ALLOWED_CMDS].join(', ')})` });
  client.publish(`${TOPIC_PREFIX}/${req.params.id}/cmd`, cmd);
  res.json({ sent: cmd });
});

// Export CSV de l'historique
app.get('/api/export.csv', async (req, res) => {
  const minutes = Number(req.query.minutes ?? 60);
  const rows = await store.queryTelemetry({
    deviceId: req.query.deviceId, since: new Date(Date.now() - minutes * 60_000), limit: 100000,
  });
  const cols = ['ts', 'deviceId', 'temperature', 'humidity', 'vibRms', 'vibPeak', 'current', 'state'];
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
  console.log(`[HTTP] API + Socket.io sur http://localhost:${PORT}`);
  checkTelegram().then(({ ok, error }) => {
    console.log(ok ? '[TELEGRAM] ✅ notifications activées (message de test envoyé sur ton téléphone)'
                   : `[TELEGRAM] ❌ ${error}`);
  });
});
