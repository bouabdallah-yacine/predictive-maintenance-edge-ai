// ============================================================================
//  Persistance : MongoDB (Mongoose) avec repli en mémoire si Mongo est absent.
//  Le repli permet de lancer la démo sans rien installer d'autre que Node.
// ============================================================================
import mongoose from 'mongoose';

const telemetrySchema = new mongoose.Schema({
  deviceId:    { type: String, index: true },
  ts:          { type: Date, default: Date.now },
  temperature: Number,
  humidity:    Number,
  vibRms:      Number,
  vibPeak:     Number,
  current:     Number,
  state:       String,          // état global calculé par le serveur
  deviceState: String,          // état annoncé par le firmware
  zscores:     Object,
  faultInjected: Boolean,
  aiScore:     Number,          // IA embarquée : score d'anomalie 0..1
  aiAnomaly:   Boolean,
  aiCause:     String,
}, { versionKey: false });
telemetrySchema.index({ deviceId: 1, ts: -1 });
// Rétention automatique : 7 jours (index TTL)
telemetrySchema.index({ ts: 1 }, { expireAfterSeconds: 7 * 24 * 3600 });

const alertSchema = new mongoose.Schema({
  deviceId: { type: String, index: true },
  ts:       { type: Date, default: Date.now },
  kind:     String,       // THRESHOLD | STATISTICAL | PREDICTIVE | AI | RECOVERY | CONNECTIVITY
  metric:   String,
  severity: String,       // INFO | WARNING | CRITICAL
  value:    Number,
  zscore:   Number,
  message:  String,
  acknowledged: { type: Boolean, default: false },
}, { versionKey: false });
alertSchema.index({ ts: -1 });

const Telemetry = mongoose.model('Telemetry', telemetrySchema);
const Alert = mongoose.model('Alert', alertSchema);

const clean = (d) => {
  if (!d) return d;
  const o = typeof d.toObject === 'function' ? d.toObject() : { ...d };
  o.id = String(o._id);
  delete o._id;
  return o;
};

class MongoStore {
  name = 'mongodb';
  async saveTelemetry(t) { return clean(await Telemetry.create(t)); }
  async queryTelemetry({ deviceId, since, limit = 2000 }) {
    const q = { ts: { $gte: since } };
    if (deviceId) q.deviceId = deviceId;
    const rows = await Telemetry.find(q).sort({ ts: 1 }).limit(limit).lean();
    return rows.map(clean);
  }
  async saveAlert(a) { return clean(await Alert.create(a)); }
  async listAlerts({ limit = 100, deviceId }) {
    const q = deviceId ? { deviceId } : {};
    return (await Alert.find(q).sort({ ts: -1 }).limit(limit).lean()).map(clean);
  }
  async ackAlert(id) {
    return clean(await Alert.findByIdAndUpdate(id, { acknowledged: true }, { new: true }).lean());
  }
  async stats(deviceId, since) {
    const [r] = await Telemetry.aggregate([
      { $match: { deviceId, ts: { $gte: since } } },
      { $group: {
        _id: null,
        n: { $sum: 1 },
        tempAvg: { $avg: '$temperature' }, tempMax: { $max: '$temperature' },
        vibAvg: { $avg: '$vibRms' },       vibMax: { $max: '$vibRms' },
        currAvg: { $avg: '$current' },     currMax: { $max: '$current' },
        criticalCount: { $sum: { $cond: [{ $eq: ['$state', 'CRITICAL'] }, 1, 0] } },
      } },
    ]);
    return r ?? null;
  }
}

class MemoryStore {
  name = 'memory';
  telemetry = [];
  alerts = [];
  seq = 0;
  async saveTelemetry(t) {
    const doc = { ...t, id: String(++this.seq) };
    this.telemetry.push(doc);
    if (this.telemetry.length > 50000) this.telemetry.shift();
    return doc;
  }
  async queryTelemetry({ deviceId, since, limit = 2000 }) {
    return this.telemetry
      .filter((t) => t.ts >= since && (!deviceId || t.deviceId === deviceId))
      .slice(-limit);
  }
  async saveAlert(a) {
    const doc = { acknowledged: false, ...a, id: String(++this.seq) };
    this.alerts.unshift(doc);
    if (this.alerts.length > 5000) this.alerts.pop();
    return doc;
  }
  async listAlerts({ limit = 100, deviceId }) {
    return this.alerts.filter((a) => !deviceId || a.deviceId === deviceId).slice(0, limit);
  }
  async ackAlert(id) {
    const a = this.alerts.find((x) => x.id === id);
    if (a) a.acknowledged = true;
    return a ?? null;
  }
  async stats(deviceId, since) {
    const rows = this.telemetry.filter((t) => t.deviceId === deviceId && t.ts >= since);
    if (!rows.length) return null;
    const avg = (k) => rows.reduce((s, r) => s + (r[k] ?? 0), 0) / rows.length;
    const max = (k) => Math.max(...rows.map((r) => r[k] ?? -Infinity));
    return {
      n: rows.length,
      tempAvg: avg('temperature'), tempMax: max('temperature'),
      vibAvg: avg('vibRms'), vibMax: max('vibRms'),
      currAvg: avg('current'), currMax: max('current'),
      criticalCount: rows.filter((r) => r.state === 'CRITICAL').length,
    };
  }
}

export async function createStore(url) {
  if (url) {
    try {
      await mongoose.connect(url, { serverSelectionTimeoutMS: 4000 });
      console.log(`[DB] MongoDB connecté (${mongoose.connection.name})`);
      return new MongoStore();
    } catch (e) {
      console.warn(`[DB] MongoDB injoignable (${e.message}) → stockage en mémoire`);
    }
  } else {
    console.warn('[DB] MONGO_URL non défini → stockage en mémoire');
  }
  return new MemoryStore();
}

export { MemoryStore };
