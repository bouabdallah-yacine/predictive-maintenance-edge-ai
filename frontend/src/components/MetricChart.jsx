import {
  CartesianGrid, Line, LineChart, ReferenceArea, ReferenceLine, ResponsiveContainer, Tooltip, XAxis, YAxis,
} from 'recharts';

const fmtTime = (t) => new Date(t).toLocaleTimeString('fr-FR', { hour: '2-digit', minute: '2-digit', second: '2-digit' });

export default function MetricChart({ title, unit, data, dataKey, color, th, secondKey }) {
  const values = data.map((d) => d[secondKey ?? dataKey]).filter((v) => typeof v === 'number');
  const maxV = Math.max(th.crit * 1.15, ...values);
  return (
    <div className="card chart">
      <div className="chart-head">
        <h3>{title}</h3>
        <span className="muted">seuils : {th.warn} / {th.crit} {unit}</span>
      </div>
      <ResponsiveContainer width="100%" height={200}>
        <LineChart data={data} margin={{ top: 8, right: 16, bottom: 0, left: -8 }}>
          <CartesianGrid stroke="var(--grid)" vertical={false} />
          <XAxis dataKey="t" type="number" domain={['dataMin', 'dataMax']} tickFormatter={fmtTime}
                 stroke="var(--muted)" fontSize={11} minTickGap={40} />
          <YAxis stroke="var(--muted)" fontSize={11} domain={[0, Math.ceil(maxV * 10) / 10]} />
          <ReferenceArea y1={th.warn} y2={th.crit} fill="#f59e0b" fillOpacity={0.07} />
          <ReferenceArea y1={th.crit} y2={maxV * 2} fill="#ef4444" fillOpacity={0.08} />
          <ReferenceLine y={th.warn} stroke="#f59e0b" strokeDasharray="4 4" />
          <ReferenceLine y={th.crit} stroke="#ef4444" strokeDasharray="4 4" />
          <Tooltip labelFormatter={fmtTime}
                   formatter={(v, name) => [`${v} ${unit}`, name]}
                   contentStyle={{ background: 'var(--card)', border: '1px solid var(--border)', borderRadius: 8 }} />
          {secondKey && (
            <Line type="monotone" dataKey={secondKey} name="crête" stroke={color} strokeOpacity={0.35}
                  dot={false} isAnimationActive={false} strokeWidth={1} />
          )}
          <Line type="monotone" dataKey={dataKey} name={title} stroke={color} dot={false}
                isAnimationActive={false} strokeWidth={2} />
        </LineChart>
      </ResponsiveContainer>
    </div>
  );
}
