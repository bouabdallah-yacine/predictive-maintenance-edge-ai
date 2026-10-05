import { Area, AreaChart, ReferenceLine, ResponsiveContainer, YAxis } from 'recharts';

// The firmware may send the cause in French ("température", "courant", ...)
// or in English: both are accepted and displayed in English.
const CAUSE = {
  temperature: 'temperature', 'température': 'temperature',
  vibration: 'vibration',
  current: 'current', courant: 'current',
  humidity: 'humidity', 'humidité': 'humidity', humidite: 'humidity',
};
const causeLabel = (c) => CAUSE[String(c).trim().toLowerCase()] ?? c;

// Edge AI verdict (neural network running on the ESP32)
export default function AiCard({ last, data }) {
  const has = typeof last?.aiScore === 'number';
  const pct = has ? Math.round(last.aiScore * 100) : null;
  const anomaly = has && last.aiAnomaly;
  return (
    <div className={`card ai ${anomaly ? 'ai-alert' : has ? 'ai-ok' : ''}`}>
      <div className="kpi-head">
        <h3>🤖 Edge AI</h3>
        {has && <span className={`tag ${anomaly ? 'WARNING' : 'NORMAL'}`}>{anomaly ? 'Anomaly' : 'Normal'}</span>}
      </div>
      {!has ? (
        <p className="muted">No AI score for this machine (firmware without TinyML, or simulator).</p>
      ) : (
        <>
          <div className="ai-score">
            <b>{pct}%</b>
            <span className="muted">anomaly score</span>
          </div>
          <div className="ai-bar" title="threshold: 50%">
            <span style={{ width: `${pct}%` }} />
            <i style={{ left: '50%' }} />
          </div>
          <p className="ai-verdict">
            {anomaly
              ? <>Behaviour never seen during healthy operation{last.aiCause && <> — probable cause: <b>{causeLabel(last.aiCause)}</b></>}</>
              : 'Measurements match the learned normal operation.'}
          </p>
          <ResponsiveContainer width="100%" height={48}>
            <AreaChart data={data.slice(-120)} margin={{ top: 2, right: 0, bottom: 0, left: 0 }}>
              <YAxis hide domain={[0, 1]} />
              <ReferenceLine y={0.5} stroke="var(--warn)" strokeDasharray="3 3" />
              <Area type="monotone" dataKey="aiScore" stroke="currentColor" fill="currentColor" fillOpacity={0.15}
                    strokeWidth={1.5} dot={false} isAnimationActive={false} />
            </AreaChart>
          </ResponsiveContainer>
          <p className="muted">4→16→16→1 neural network running on the ESP32 (TinyML)</p>
        </>
      )}
    </div>
  );
}
