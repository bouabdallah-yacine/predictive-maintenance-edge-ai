const TEXT = {
  NORMAL: ['✅', 'Fonctionnement normal'],
  WARNING: ['⚠️', 'Attention : paramètre hors plage'],
  CRITICAL: ['🚨', 'Anomalie critique : intervention requise'],
};

export default function StatusBanner({ last, online }) {
  if (!last) return null;
  if (online === false) {
    return <div className="banner OFFLINE">📡 Machine hors ligne : dernières données à {new Date(last.ts).toLocaleTimeString('fr-FR')}</div>;
  }
  const [icon, text] = TEXT[last.state] ?? TEXT.NORMAL;
  const causes = Object.entries(last.levels ?? {})
    .filter(([, l]) => l !== 'NORMAL')
    .map(([m]) => ({ temperature: 'température', vibRms: 'vibration', current: 'courant' }[m] ?? m));
  return (
    <div className={`banner ${last.state}`}>
      {icon} {text}{causes.length > 0 && ` (${causes.join(', ')})`}
      {last.faultInjected && <span className="sim">panne simulée</span>}
    </div>
  );
}
