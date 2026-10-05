// ============================================================================
//  Machine simulator — publishes exactly the same JSON as the ESP32.
//  Lets you develop / demo the backend and the dashboard without Wokwi.
//
//  Usage:
//    node simulator.js                       → "demo" scenario (runs through everything)
//    node simulator.js --scenario overheat   → normal | overheat | bearing | overcurrent | demo
//    node simulator.js --devices 3           → several machines
//  Keys while running: [n]ormal [o]verheat [b]earing [c]urrent [d]emo [q]uit
// ============================================================================
import mqtt from 'mqtt';

const args = Object.fromEntries(
  process.argv.slice(2).reduce((acc, a, i, arr) => (a.startsWith('--') ? [...acc, [a.slice(2), arr[i + 1]]] : acc), []),
);
const MQTT_URL     = process.env.MQTT_URL ?? args.url ?? 'mqtt://broker.hivemq.com:1883';
const TOPIC_PREFIX = process.env.TOPIC_PREFIX ?? args.prefix ?? 'pfe-monitor-7f3a';
const N_DEVICES    = Number(args.devices ?? 1);
let scenario       = args.scenario ?? 'demo';

const gauss = () => Math.sqrt(-2 * Math.log(Math.random() || 1e-9)) * Math.cos(2 * Math.PI * Math.random());
const round = (x, d) => Math.round(x * 10 ** d) / 10 ** d;
const lvl = (v, w, c) => (v >= c ? 'CRITICAL' : v >= w ? 'WARNING' : 'NORMAL');

class Machine {
  constructor(id) {
    this.id = id;
    this.seq = 0;
    this.t0 = Date.now();
    this.temp = 38 + Math.random() * 4;   // thermal state (inertia)
    this.wear = 0;                         // bearing wear 0..1
    this.fault = false;                    // MQTT fault_on command
    this.phaseStart = Date.now();
  }

  currentScenario() {
    if (this.fault) return 'overcurrent';
    if (scenario !== 'demo') return scenario;
    // Demo: 4-minute cycle — normal, overheating, bearing, overcurrent
    const s = ((Date.now() - this.t0) / 1000) % 240;
    return s < 60 ? 'normal' : s < 120 ? 'overheat' : s < 180 ? 'bearing' : s < 210 ? 'overcurrent' : 'normal';
  }

  step() {
    const sc = this.currentScenario();
    // Thermal: first-order response towards a target temperature
    const target = sc === 'overheat' ? 90 : sc === 'overcurrent' ? 55 : 40;
    this.temp += (target - this.temp) * (sc === 'overheat' ? 0.02 : 0.05) + gauss() * 0.15;

    this.wear = sc === 'bearing' ? Math.min(this.wear + 0.015, 1) : Math.max(this.wear - 0.05, 0);
    const vibRms = Math.abs(0.05 + this.wear * 0.75 + gauss() * 0.01);
    const vibPeak = vibRms * (1.4 + Math.random() * 0.3 + this.wear);

    let current = 1.6 + gauss() * 0.05 + this.wear * 0.6;
    if (sc === 'overcurrent') current += 3.2 + gauss() * 0.2;

    const levels = {
      temperature: lvl(this.temp, 60, 75),
      vibration: lvl(vibRms, 0.3, 0.6),
      current: lvl(current, 3.5, 4.5),
    };
    const order = ['NORMAL', 'WARNING', 'CRITICAL'];
    const state = order[Math.max(...Object.values(levels).map((l) => order.indexOf(l)))];

    return {
      deviceId: this.id,
      seq: this.seq++,
      uptimeMs: Date.now() - this.t0,
      temperature: round(this.temp, 1),
      humidity: round(45 + gauss() * 1.5, 1),
      vibRms: round(vibRms, 3),
      vibPeak: round(vibPeak, 3),
      current: round(Math.max(current, 0), 2),
      state,
      faultInjected: sc !== 'normal',
      levels,
      health: { dht: true, mpu: true },
      _scenario: sc,
    };
  }
}

const machines = Array.from({ length: N_DEVICES }, (_, i) => new Machine(`machine${String(i + 1).padStart(2, '0')}`));
const client = mqtt.connect(MQTT_URL, {
  clientId: `simulator-${Math.random().toString(16).slice(2, 8)}`,
  will: { topic: `${TOPIC_PREFIX}/${machines[0].id}/status`, payload: 'offline', retain: true },
  // Private broker: username / password (empty = public broker)
  username: process.env.MQTT_USERNAME || undefined,
  password: process.env.MQTT_PASSWORD || undefined,
});

client.on('connect', () => {
  console.log(`[SIM] connected to ${MQTT_URL} — prefix "${TOPIC_PREFIX}" — ${N_DEVICES} machine(s) — scenario ${scenario}`);
  for (const m of machines) {
    client.publish(`${TOPIC_PREFIX}/${m.id}/status`, 'online', { retain: true });
    client.subscribe(`${TOPIC_PREFIX}/${m.id}/cmd`);
  }
});
client.on('error', (e) => console.error('[SIM] MQTT error:', e.message));

client.on('message', (topic, payload) => {
  const id = topic.split('/')[1];
  const m = machines.find((x) => x.id === id);
  const cmd = payload.toString();
  if (!m) return;
  if (cmd === 'fault_on') m.fault = true;
  if (cmd === 'fault_off') m.fault = false;
  console.log(`[SIM] ${id} command received: ${cmd}`);
});

setInterval(() => {
  if (!client.connected) return;
  for (const m of machines) {
    const msg = m.step();
    const { _scenario, ...payload } = msg;
    client.publish(`${TOPIC_PREFIX}/${m.id}/telemetry`, JSON.stringify(payload));
    if (m === machines[0]) {
      process.stdout.write(
        `\r[${_scenario.padEnd(11)}] T=${payload.temperature.toFixed(1)}°C  V=${payload.vibRms.toFixed(3)}g  I=${payload.current.toFixed(2)}A  → ${payload.state.padEnd(8)}`,
      );
    }
  }
}, 1000);

// Keyboard control
if (process.stdin.isTTY) {
  process.stdin.setRawMode(true);
  process.stdin.resume();
  process.stdin.on('data', (k) => {
    const key = k.toString();
    const map = { n: 'normal', o: 'overheat', b: 'bearing', c: 'overcurrent', d: 'demo' };
    if (map[key]) { scenario = map[key]; console.log(`\n[SIM] scenario → ${scenario}`); }
    if (key === 'q' || key === '\u0003') {
      client.publish(`${TOPIC_PREFIX}/${machines[0].id}/status`, 'offline', { retain: true }, () => process.exit(0));
    }
  });
}
