import { Line, LineChart, ResponsiveContainer, YAxis } from 'recharts';

const LABEL = { NORMAL: 'Normal', WARNING: 'Warning', CRITICAL: 'Critical' };

export default function KpiCard({ label, value, unit, digits = 1, level, data, dataKey, extra }) {
  const lv = level ?? 'NONE';
  return (
    <div className={`card kpi lvl-${lv}`}>
      <div className="kpi-head">
        <span>{label}</span>
        {level && <span className={`tag ${level}`}>{LABEL[level]}</span>}
      </div>
      <div className="kpi-value">
        {typeof value === 'number' ? value.toFixed(digits) : '—'}
        <small>{unit}</small>
      </div>
      <div className="kpi-foot">{extra ?? ' '}</div>
      <div className="spark">
        <ResponsiveContainer width="100%" height={36}>
          <LineChart data={data.slice(-60)}>
            <YAxis hide domain={['auto', 'auto']} />
            <Line type="monotone" dataKey={dataKey} dot={false} strokeWidth={1.5} stroke="currentColor" isAnimationActive={false} />
          </LineChart>
        </ResponsiveContainer>
      </div>
    </div>
  );
}
