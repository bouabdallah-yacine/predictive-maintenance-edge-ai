import { useEffect, useMemo, useRef, useState } from 'react';
import { io } from 'socket.io-client';
import { api, API_URL } from './api.js';
import KpiCard from './components/KpiCard.jsx';
import MetricChart from './components/MetricChart.jsx';
import AlertList from './components/AlertList.jsx';
import StatusBanner from './components/StatusBanner.jsx';
import AiCard from './components/AiCard.jsx';

const MAX_POINTS = 300;          // 5 min à 1 Hz
const DEFAULT_THRESHOLDS = {
  temperature: { warn: 60, crit: 75, unit: '°C' },
  vibRms: { warn: 0.3, crit: 0.6, unit: 'g' },
  current: { warn: 3.5, crit: 4.5, unit: 'A' },
};

export default function App() {
  const [thresholds, setThresholds] = useState(DEFAULT_THRESHOLDS);
  const [devices, setDevices] = useState({});        // id → { online, lastSeen }
  const [selected, setSelected] = useState(null);
  const [series, setSeries] = useState({});          // id → [points]
  const [latest, setLatest] = useState({});          // id → dernier point enrichi
  const [alerts, setAlerts] = useState([]);
  const [conn, setConn] = useState({ socket: false, broker: false });
  const selectedRef = useRef(null);
  selectedRef.current = selected;

  // Chargement initial + temps réel
  useEffect(() => {
    api.config().then((c) => setThresholds(c.thresholds)).catch(() => {});
    api.alerts(50).then(setAlerts).catch(() => {});
    api.devices().then((list) => {
      setDevices(Object.fromEntries(list.map((d) => [d.deviceId, d])));
      if (list[0]) setSelected((s) => s ?? list[0].deviceId);
    }).catch(() => {});

    const socket = io(API_URL, { transports: ['websocket', 'polling'] });
    socket.on('connect', () => setConn((c) => ({ ...c, socket: true })));
    socket.on('disconnect', () => setConn((c) => ({ ...c, socket: false })));
    socket.on('broker', ({ connected }) => setConn((c) => ({ ...c, broker: connected })));
    socket.on('devices', (list) => setDevices((prev) => ({ ...prev, ...Object.fromEntries(list.map((d) => [d.deviceId, d])) })));
    socket.on('device', (d) => setDevices((prev) => ({ ...prev, [d.deviceId]: { ...prev[d.deviceId], ...d } })));

    socket.on('telemetry', (t) => {
      setSelected((s) => s ?? t.deviceId);
      setDevices((prev) => ({ ...prev, [t.deviceId]: { ...prev[t.deviceId], deviceId: t.deviceId, online: true, lastSeen: Date.now() } }));
      setLatest((prev) => ({ ...prev, [t.deviceId]: t }));
      setSeries((prev) => {
        const arr = [...(prev[t.deviceId] ?? []), toPoint(t)];
        return { ...prev, [t.deviceId]: arr.slice(-MAX_POINTS) };
      });
    });
    socket.on('alert', (a) => setAlerts((prev) => [a, ...prev].slice(0, 100)));
    socket.on('alert-updated', (a) => setAlerts((prev) => prev.map((x) => (x.id === a.id ? a : x))));
    return () => socket.close();
  }, []);

  // Historique lors du changement de machine
  useEffect(() => {
    if (!selected) return;
    api.telemetry(selected, 5).then((rows) => {
      setSeries((prev) => ({ ...prev, [selected]: rows.map(toPoint).slice(-MAX_POINTS) }));
    }).catch(() => {});
  }, [selected]);

  const data = series[selected] ?? [];
  const last = latest[selected];
  const deviceList = useMemo(() => Object.values(devices).sort((a, b) => a.deviceId.localeCompare(b.deviceId)), [devices]);
  const deviceAlerts = alerts.filter((a) => !selected || a.deviceId === selected);
  const unacked = deviceAlerts.filter((a) => !a.acknowledged && a.severity !== 'INFO').length;

  const sendCmd = (cmd) => selected && api.cmd(selected, cmd).catch((e) => alert(e.message));
  const ack = (id) => api.ack(id).catch(() => {});

  return (
    <div className="app">
      <header className="topbar">
        <div className="brand">
          <span className="logo">⚙</span>
          <div>
            <h1>Machine Monitor</h1>
            <p>Surveillance industrielle &amp; détection d'anomalies</p>
          </div>
        </div>
        <div className="conn">
          <Dot ok={conn.socket} label="Serveur" />
          <Dot ok={conn.broker} label="Broker MQTT" />
        </div>
      </header>

      <nav className="devices">
        {deviceList.length === 0 && <span className="muted">En attente d'une machine… (lance Wokwi ou le simulateur)</span>}
        {deviceList.map((d) => (
          <button key={d.deviceId}
                  className={`device ${d.deviceId === selected ? 'active' : ''}`}
                  onClick={() => setSelected(d.deviceId)}>
            <span className={`pill ${d.online ? 'on' : 'off'}`} />
            {d.deviceId}
            {latest[d.deviceId] && <span className={`tag ${latest[d.deviceId].state}`}>{latest[d.deviceId].state}</span>}
          </button>
        ))}
      </nav>

      <StatusBanner last={last} online={devices[selected]?.online} />

      <section className="kpis">
        <KpiCard label="Température" value={last?.temperature} unit="°C" digits={1}
                 level={last?.levels?.temperature} data={data} dataKey="temperature" />
        <KpiCard label="Humidité" value={last?.humidity} unit="%" digits={1} data={data} dataKey="humidity" />
        <KpiCard label="Vibration (RMS)" value={last?.vibRms} unit="g" digits={3}
                 level={last?.levels?.vibRms} data={data} dataKey="vibRms" extra={last?.zscores?.vibRms != null ? `z = ${last.zscores.vibRms}` : null} />
        <KpiCard label="Courant" value={last?.current} unit="A" digits={2}
                 level={last?.levels?.current} data={data} dataKey="current" />
      </section>

      <main className="grid">
        <div className="charts">
          <MetricChart title="Température" unit="°C" data={data} dataKey="temperature" color="#f59e0b" th={thresholds.temperature} />
          <MetricChart title="Vibration RMS" unit="g" data={data} dataKey="vibRms" color="#38bdf8" th={thresholds.vibRms} secondKey="vibPeak" />
          <MetricChart title="Courant moteur" unit="A" data={data} dataKey="current" color="#a78bfa" th={thresholds.current} />
        </div>

        <aside className="side">
          <AiCard last={last} data={data} />
          <div className="card controls">
            <h3>Commandes</h3>
            <div className="btns">
              <button onClick={() => sendCmd('fault_on')} className="danger">Injecter une panne</button>
              <button onClick={() => sendCmd('fault_off')}>Arrêter la panne</button>
              <button onClick={() => sendCmd('mute')}>Couper le buzzer</button>
              <a className="button" href={selected ? api.exportUrl(selected) : '#'}>Export CSV (1 h)</a>
            </div>
            {last?.etaCriticalMin != null && (
              <p className="eta">🔮 Surchauffe estimée dans <b>{last.etaCriticalMin} min</b></p>
            )}
          </div>
          <AlertList alerts={deviceAlerts} unacked={unacked} onAck={ack} />
        </aside>
      </main>
    </div>
  );
}

function toPoint(t) {
  return {
    t: new Date(t.ts).getTime(),
    temperature: t.temperature,
    humidity: t.humidity,
    vibRms: t.vibRms,
    vibPeak: t.vibPeak,
    current: t.current,
    state: t.state,
    aiScore: t.aiScore,
  };
}

function Dot({ ok, label }) {
  return <span className="dot"><span className={`pill ${ok ? 'on' : 'off'}`} />{label}</span>;
}
