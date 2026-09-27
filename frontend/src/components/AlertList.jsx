const ICON = { CRITICAL: '🚨', WARNING: '⚠️', INFO: 'ℹ️' };
const KIND = {
  THRESHOLD: 'Seuil', STATISTICAL: 'Statistique', PREDICTIVE: 'Prédictif',
  RECOVERY: 'Rétablissement', CONNECTIVITY: 'Connexion',
};

export default function AlertList({ alerts, unacked, onAck }) {
  return (
    <div className="card alerts">
      <h3>Alertes {unacked > 0 && <span className="badge">{unacked}</span>}</h3>
      {alerts.length === 0 && <p className="muted">Aucune alerte.</p>}
      <ul>
        {alerts.map((a) => (
          <li key={a.id} className={`alert sev-${a.severity} ${a.acknowledged ? 'acked' : ''}`}>
            <span className="icon">{ICON[a.severity] ?? '•'}</span>
            <div className="body">
              <div className="msg">{a.message}</div>
              <div className="meta">
                {new Date(a.ts).toLocaleTimeString('fr-FR')} · {a.deviceId} · {KIND[a.kind] ?? a.kind}
              </div>
            </div>
            {!a.acknowledged && a.severity !== 'INFO' && (
              <button className="ack" onClick={() => onAck(a.id)} title="Acquitter">✓</button>
            )}
          </li>
        ))}
      </ul>
    </div>
  );
}
