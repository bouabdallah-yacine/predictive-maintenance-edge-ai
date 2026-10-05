import { useEffect, useMemo, useRef, useState } from 'react';
import { io } from 'socket.io-client';
import { api, API_URL } from './api.js';
import KpiCard from './components/KpiCard.jsx';
import MetricChart from './components/MetricChart.jsx';
import AlertList from './components/AlertList.jsx';
import StatusBanner from './components/StatusBanner.jsx';
import AiCard from './components/AiCard.jsx';

const MAX_POINTS = 300;          // 5 min at 1 Hz
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
  const [latest, setLatest] = useState({});          // id → latest enriched point
  const [alerts, setAlerts] = useState([]);
  const [conn, setConn] = useState({ socket: false, broker: false });
  const selectedRef = useRef(null);
  selectedRef.current = selected;

  // Initial load + real time
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

  // Load history when the selected machine changes
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
            <p>Industrial monitoring &amp; anomaly detection</p>
          </div>
        </div>
        <div className="conn">
          <Dot ok={conn.socket} label="Server" />
          <Dot ok={conn.broker} label="Broker MQTT" />
          <ThemeToggle />
        </div>
      </header>

      <nav className="devices">
        {deviceList.length === 0 && <span className="muted">Waiting for a machine… (start Wokwi or the simulator)</span>}
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
        <KpiCard label="Temperature" value={last?.temperature} unit="°C" digits={1}
                 level={last?.levels?.temperature} data={data} dataKey="temperature" />
        <KpiCard label="Humidity" value={last?.humidity} unit="%" digits={1} data={data} dataKey="humidity" />
        <KpiCard label="Vibration (RMS)" value={last?.vibRms} unit="g" digits={3}
                 level={last?.levels?.vibRms} data={data} dataKey="vibRms" extra={last?.zscores?.vibRms != null ? `z = ${last.zscores.vibRms}` : null} />
        <KpiCard label="Current" value={last?.current} unit="A" digits={2}
                 level={last?.levels?.current} data={data} dataKey="current" />
      </section>

      <main className="grid">
        <div className="charts">
          <MetricChart title="Temperature" unit="°C" data={data} dataKey="temperature" color="var(--c-temp)" th={thresholds.temperature} />
          <MetricChart title="Vibration RMS" unit="g" data={data} dataKey="vibRms" color="var(--c-vib)" th={thresholds.vibRms} secondKey="vibPeak" />
          <MetricChart title="Motor current" unit="A" data={data} dataKey="current" color="var(--c-cur)" th={thresholds.current} />
        </div>

        <aside className="side">
          <AiCard last={last} data={data} />
          <div className="card controls">
            <h3>Commands</h3>
            <div className="btns">
              <button onClick={() => sendCmd('fault_on')} className="danger">Inject a fault</button>
              <button onClick={() => sendCmd('fault_off')}>Stop the fault</button>
              <button onClick={() => sendCmd('mute')}>Mute the buzzer</button>
              <a className="button" href={selected ? api.exportUrl(selected) : '#'}>Export CSV (1 h)</a>
            </div>
            {last?.etaCriticalMin != null && (
              <p className="eta">🔮 Overheating expected in <b>{last.etaCriticalMin} min</b></p>
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

// Light / dark theme: follows the system; the button forces one or the other (remembered)
function ThemeToggle() {
  const sys = () => (window.matchMedia?.('(prefers-color-scheme: light)').matches ? 'light' : 'dark');
  const [theme, setTheme] = useState(() => {
    try { return localStorage.getItem('theme') || sys(); } catch { return sys(); }
  });
  useEffect(() => {
    document.documentElement.dataset.theme = theme;
    try { localStorage.setItem('theme', theme); } catch { /* private browsing */ }
  }, [theme]);
  const next = theme === 'dark' ? 'light' : 'dark';
  return (
    <button id="theme" onClick={() => setTheme(next)} title={next === 'light' ? 'Switch to light mode' : 'Switch to dark mode'}
            aria-label="Toggle theme">{theme === 'dark' ? '☀' : '☾'}</button>
  );
}

function Dot({ ok, label }) {
  return <span className="dot"><span className={`pill ${ok ? 'on' : 'off'}`} />{label}</span>;
}
