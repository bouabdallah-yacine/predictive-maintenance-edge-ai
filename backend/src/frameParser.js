// ============================================================================
//  Parseur des trames UART du STM32 — portage JS de firmware/stm32/.../protocol.c
//  $MM,<seq>,<temp×10>,<hum×10>,<vib mg>,<crête mg>,<courant mA>,<flags>*<XOR hex>
// ============================================================================
const LEVELS = ['NORMAL', 'WARNING', 'CRITICAL'];

export class FrameParser {
  constructor() {
    this.state = 'WAIT';
    this.body = '';
    this.cs = '';
    this.stats = { ok: 0, errChecksum: 0, errFormat: 0 };
  }

  /** Donne un caractère ; renvoie un objet télémétrie quand une trame valide est complète. */
  feed(ch) {
    if (ch === '$') { this.state = 'BODY'; this.body = ''; this.cs = ''; return null; }
    if (this.state === 'BODY') {
      if (ch === '*') this.state = 'CS';
      else if (ch === '\r' || ch === '\n' || this.body.length > 94) { this.stats.errFormat++; this.state = 'WAIT'; }
      else this.body += ch;
    } else if (this.state === 'CS') {
      this.cs += ch;
      if (this.cs.length === 2) { this.state = 'WAIT'; return this.#finish(); }
    }
    return null;
  }

  #finish() {
    let x = 0;
    for (const c of this.body) x ^= c.charCodeAt(0);
    if (parseInt(this.cs, 16) !== x) { this.stats.errChecksum++; return null; }
    const [tag, ...f] = this.body.split(',');
    const v = f.map((s) => (/^-?\d+$/.test(s) ? Number(s) : NaN));
    if (tag !== 'MM' || v.length !== 7 || v.some(Number.isNaN)) { this.stats.errFormat++; return null; }
    const [seq, tempD, humD, vib, peak, curr, flags] = v;
    this.stats.ok++;
    return {
      seq,
      temperature: tempD / 10,
      humidity: humD / 10,
      vibRms: vib / 1000,
      vibPeak: peak / 1000,
      current: curr / 1000,
      state: LEVELS[Math.min((flags >> 4) & 3, 2)],
      faultInjected: Boolean(flags & 4),
      health: { dht: Boolean(flags & 1), mpu: Boolean(flags & 2) },
    };
  }
}

/**
 * Filtre Telnet : le port série de Wokwi est exposé en RFC2217 (protocole
 * Telnet). On retire les séquences de négociation (octet IAC = 255) pour ne
 * garder que les données.
 */
export class TelnetFilter {
  constructor() { this.st = 'DATA'; }
  push(buf) {
    let out = '';
    for (const b of buf) {
      switch (this.st) {
        case 'DATA': if (b === 255) this.st = 'IAC'; else out += String.fromCharCode(b); break;
        case 'IAC':
          if (b === 255) { out += '\xff'; this.st = 'DATA'; }
          else if (b >= 251 && b <= 254) this.st = 'OPT';    // WILL/WONT/DO/DONT <option>
          else if (b === 250) this.st = 'SB';                // sous-négociation
          else this.st = 'DATA';
          break;
        case 'OPT': this.st = 'DATA'; break;
        case 'SB': if (b === 255) this.st = 'SB_IAC'; break;
        case 'SB_IAC': this.st = b === 240 ? 'DATA' : 'SB'; break;  // IAC SE
      }
    }
    return out;
  }
}
