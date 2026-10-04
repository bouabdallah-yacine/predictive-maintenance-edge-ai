import { Area, AreaChart, ReferenceLine, ResponsiveContainer, YAxis } from 'recharts';

const CAUSE = { temperature: 'température', vibration: 'vibration', courant: 'courant' };

// Verdict de l'IA embarquée (réseau de neurones exécuté dans l'ESP32)
export default function AiCard({ last, data }) {
  const has = typeof last?.aiScore === 'number';
  const pct = has ? Math.round(last.aiScore * 100) : null;
  const anomaly = has && last.aiAnomaly;
  return (
    <div className={`card ai ${anomaly ? 'ai-alert' : has ? 'ai-ok' : ''}`}>
      <div className="kpi-head">
        <h3>🤖 IA embarquée</h3>
        {has && <span className={`tag ${anomaly ? 'WARNING' : 'NORMAL'}`}>{anomaly ? 'Anomalie' : 'Normal'}</span>}
      </div>
      {!has ? (
        <p className="muted">Pas de score IA pour cette machine (firmware sans TinyML ou simulateur).</p>
      ) : (
        <>
          <div className="ai-score">
            <b>{pct} %</b>
            <span className="muted">score d'anomalie</span>
          </div>
          <div className="ai-bar" title="seuil : 50 %">
            <span style={{ width: `${pct}%` }} />
            <i style={{ left: '50%' }} />
          </div>
          <p className="ai-verdict">
            {anomaly
              ? <>Comportement jamais vu en fonctionnement sain{last.aiCause && <> — cause probable : <b>{CAUSE[last.aiCause] ?? last.aiCause}</b></>}</>
              : 'Les mesures correspondent au fonctionnement normal appris.'}
          </p>
          <ResponsiveContainer width="100%" height={48}>
            <AreaChart data={data.slice(-120)} margin={{ top: 2, right: 0, bottom: 0, left: 0 }}>
              <YAxis hide domain={[0, 1]} />
              <ReferenceLine y={0.5} stroke="var(--warn)" strokeDasharray="3 3" />
              <Area type="monotone" dataKey="aiScore" stroke="currentColor" fill="currentColor" fillOpacity={0.15}
                    strokeWidth={1.5} dot={false} isAnimationActive={false} />
            </AreaChart>
          </ResponsiveContainer>
          <p className="muted">Réseau de neurones 4→16→16→1 exécuté sur l'ESP32 (TinyML)</p>
        </>
      )}
    </div>
  );
}
