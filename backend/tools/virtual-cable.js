// ============================================================================
//  Câble UART virtuel entre deux simulations Wokwi
//
//    Simulation STM32 (port 4100)  ⇄  ce programme  ⇄  Simulation ESP32 (port 4200)
//
//  Chaque simulation expose son port série en RFC2217 (Telnet). On retire la
//  négociation Telnet et on recopie les octets utiles dans les deux sens,
//  exactement comme les fils TX/RX d'un vrai câble.
//
//  Usage (dossier backend) :  npm run cable
//  Options : --stm32 4100 --esp32 4200
// ============================================================================
import net from 'node:net';
import { TelnetFilter } from '../src/frameParser.js';

const arg = (k, d) => { const i = process.argv.indexOf(`--${k}`); return i > 0 ? process.argv[i + 1] : d; };
const PORTS = { STM32: Number(arg('stm32', 4100)), ESP32: Number(arg('esp32', 4200)) };

const ends = {};
const stats = { 'STM32→ESP32': 0, 'ESP32→STM32': 0 };

// Telnet : l'octet 255 dans les données doit être doublé (IAC IAC)
const escapeIac = (s) => Buffer.from(s, 'latin1').toString('latin1').replace(/\xff/g, '\xff\xff');

function connect(name) {
  const other = name === 'STM32' ? 'ESP32' : 'STM32';
  const telnet = new TelnetFilter();
  const sock = net.createConnection({ host: 'localhost', port: PORTS[name] });
  let line = '';

  sock.on('connect', () => {
    ends[name] = sock;
    console.log(`[CÂBLE] ${name} connecté (port ${PORTS[name]})${ends[other] ? ' — câble branché des deux côtés ✅' : ''}`);
  });

  sock.on('data', (buf) => {
    const text = telnet.push(buf);
    const replies = telnet.takeReplies();
    if (replies.length) sock.write(replies);
    if (!text) return;
    const dest = ends[other];
    if (dest) {
      dest.write(Buffer.from(escapeIac(text), 'latin1'));
      stats[`${name}→${other}`] += text.length;
    }
    // Affiche les messages texte ("# ...") des deux cartes
    for (const ch of text) {
      if (ch === '\n') { if (line.startsWith('#')) console.log(`[${name}] ${line.trim()}`); line = ''; }
      else if (ch !== '\r') line += ch;
    }
  });

  sock.on('error', (e) => {
    if (e.code === 'ECONNREFUSED') console.log(`[CÂBLE] ${name} injoignable (port ${PORTS[name]}) — simulation lancée et visible ?`);
    else console.log(`[CÂBLE] ${name} : ${e.message}`);
  });

  sock.on('close', () => {
    if (ends[name] === sock) delete ends[name];
    setTimeout(() => connect(name), 2000);       // rebranchement automatique
  });
}

connect('STM32');
connect('ESP32');

setInterval(() => {
  const s = Object.entries(stats).map(([k, v]) => `${k}: ${v} o`).join('  |  ');
  process.stdout.write(`\r[CÂBLE] ${s}   `);
}, 2000);
