const TEXT = {
  NORMAL: ['✅', 'Normal operation'],
  WARNING: ['⚠️', 'Warning: parameter out of range'],
  CRITICAL: ['🚨', 'Critical anomaly: intervention required'],
};

export default function StatusBanner({ last, online }) {
  if (!last) return null;
  if (online === false) {
    return <div className="banner OFFLINE">📡 Machine offline: last data received at {new Date(last.ts).toLocaleTimeString('en-GB')}</div>;
  }
  const [icon, text] = TEXT[last.state] ?? TEXT.NORMAL;
  const causes = Object.entries(last.levels ?? {})
    .filter(([, l]) => l !== 'NORMAL')
    .map(([m]) => ({ temperature: 'temperature', vibRms: 'vibration', current: 'current' }[m] ?? m));
  return (
    <div className={`banner ${last.state}`}>
      {icon} {text}{causes.length > 0 && ` (${causes.join(', ')})`}
      {last.faultInjected && <span className="sim">simulated fault</span>}
    </div>
  );
}
