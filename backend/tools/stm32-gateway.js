// ============================================================================
//  Passerelle STM32 (Wokwi) → MQTT
//  Joue le rôle de l'ESP32 : lit les trames UART du STM32 simulé (exposées par
//  Wokwi sur localhost:4100) et les publie en MQTT, au même format que l'ESP32.
//  Les commandes du dashboard (fault_on / fault_off) sont renvoyées au STM32.
//
//  Usage (depuis le dossier backend) :
//    node tools/stm32-gateway.js                 → localhost:4100, device stm32-01
//    node tools/stm32-gateway.js --dry-run       → affiche sans publier
// ============================================================================
import 'dotenv/config';
import net from 'node:net';
import { FrameParser, TelnetFilter } from '../src/frameParser.js';

const arg = (k, d) => { const i = process.argv.indexOf(`--${k}`); return i > 0 ? process.argv[i + 1] : d; };
const HOST   = arg('host', 'localhost');
const PORT   = Number(arg('port', 4100));
const DEVICE = arg('device', 'stm32-01');
const PREFIX = process.env.TOPIC_PREFIX ?? 'pfe-monitor-7f3a';
const MQTT_URL = process.env.MQTT_URL ?? 'mqtt://broker.hivemq.com:1883';
const DRY = process.argv.includes('--dry-run');

let mqttClient = null;
let socket = null;

if (!DRY) {
  const mqtt = (await import('mqtt')).default;
  mqttClient = mqtt.connect(MQTT_URL, {
    clientId: `gw-${DEVICE}-${Math.random().toString(16).slice(2, 6)}`,
    will: { topic: `${PREFIX}/${DEVICE}/status`, payload: 'offline', retain: true },
  });
  mqttClient.on('connect', () => {
    console.log(`[MQTT] connecté à ${MQTT_URL} → ${PREFIX}/${DEVICE}/telemetry`);
    mqttClient.subscribe(`${PREFIX}/${DEVICE}/cmd`);
  });
  mqttClient.on('message', (_t, payload) => {
    const cmd = payload.toString();
    const out = { fault_on: 'F1\n', fault_off: 'F0\n' }[cmd];
    if (out && socket) { socket.write(out); console.log(`\n[CMD] ${cmd} → STM32`); }
  });
  mqttClient.on('error', (e) => console.error('[MQTT] erreur :', e.message));
}

function connect() {
  const parser = new FrameParser();
  const telnet = new TelnetFilter();
  let lastSeq = null;
  let line = '';
  let bytes = 0;      // octets bruts reçus (négociation comprise)
  let textBytes = 0;  // caractères utiles venant du STM32
  const t0 = Date.now();
  // Aide au diagnostic : connecté mais rien reçu au bout de 5 s
  const hint = setTimeout(() => {
    if (textBytes === 0) console.log(`[UART] connecté mais aucune donnée du STM32 (${bytes} octets de négociation reçus) : la simulation Wokwi tourne-t-elle (chronomètre qui avance, onglet visible) ?`);
  }, 5000);

  socket = net.createConnection({ host: HOST, port: PORT });
  socket.on('connect', () => {
    console.log(`[UART] connecté au STM32 simulé (${HOST}:${PORT})`);
    mqttClient?.publish(`${PREFIX}/${DEVICE}/status`, 'online', { retain: true });
  });
  socket.on('data', (buf) => {
    bytes += buf.length;
    const text = telnet.push(buf);
    const replies = telnet.takeReplies();
    if (replies.length) socket.write(replies);      // réponses de négociation Telnet
    textBytes += text.length;
    for (const ch of text) {
      // Affiche les messages texte du STM32 (lignes commençant par #)
      if (ch === '\n') { if (line.startsWith('#')) console.log(`\n[STM32] ${line.trim()}`); line = ''; }
      else line += ch;

      const frame = parser.feed(ch);
      if (!frame) continue;
      if (lastSeq !== null && frame.seq !== (lastSeq + 1) % 65536) {
        console.log(`\n[UART] ⚠ trame(s) perdue(s) : ${lastSeq} → ${frame.seq}`);
      }
      lastSeq = frame.seq;
      const payload = { deviceId: DEVICE, uptimeMs: Date.now() - t0, ...frame };
      mqttClient?.publish(`${PREFIX}/${DEVICE}/telemetry`, JSON.stringify(payload));
      process.stdout.write(
        `\r#${frame.seq}  T=${frame.temperature.toFixed(1)}°C  V=${frame.vibRms.toFixed(3)}g  I=${frame.current.toFixed(2)}A  → ${frame.state.padEnd(8)}  (ok=${parser.stats.ok} err=${parser.stats.errChecksum + parser.stats.errFormat})`,
      );
    }
  });
  socket.on('error', (e) => console.log(`[UART] ${HOST}:${PORT} injoignable (${e.code}) — la simulation Wokwi STM32 est-elle lancée ?`));
  socket.on('close', () => {
    clearTimeout(hint);
    if (bytes > 0 || parser.stats.ok > 0) console.log(`\n[UART] connexion fermée (${bytes} octets reçus) — reconnexion...`);
    socket = null;
    mqttClient?.publish(`${PREFIX}/${DEVICE}/status`, 'offline', { retain: true });
    setTimeout(connect, 2000);        // reconnexion automatique
  });
}

connect();
