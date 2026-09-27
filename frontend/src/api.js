export const API_URL = import.meta.env.VITE_API_URL ?? 'http://localhost:4000';

async function req(path, opts) {
  const r = await fetch(`${API_URL}${path}`, {
    headers: { 'Content-Type': 'application/json' },
    ...opts,
  });
  if (!r.ok) throw new Error(`${r.status} ${await r.text()}`);
  return r.json();
}

export const api = {
  health: () => req('/api/health'),
  config: () => req('/api/config'),
  devices: () => req('/api/devices'),
  telemetry: (deviceId, minutes = 5) => req(`/api/telemetry?deviceId=${deviceId}&minutes=${minutes}`),
  alerts: (limit = 50) => req(`/api/alerts?limit=${limit}`),
  stats: (deviceId, minutes = 60) => req(`/api/stats/${deviceId}?minutes=${minutes}`),
  ack: (id) => req(`/api/alerts/${id}/ack`, { method: 'POST' }),
  cmd: (deviceId, cmd) => req(`/api/devices/${deviceId}/cmd`, { method: 'POST', body: JSON.stringify({ cmd }) }),
  exportUrl: (deviceId, minutes = 60) => `${API_URL}/api/export.csv?deviceId=${deviceId}&minutes=${minutes}`,
};
