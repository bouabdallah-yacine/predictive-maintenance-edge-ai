// ============================================================================
//  Virtual UART cable between two Wokwi simulations
//
//    STM32 simulation (port 4100)  ⇄  this program  ⇄  ESP32 simulation (port 4200)
//
//  Each simulation exposes its serial port over RFC2217 (Telnet). The Telnet
//  negotiation is stripped and the payload bytes are copied in both directions,
//  exactly like the TX/RX wires of a real cable.
//
//  Usage (backend folder):  npm run cable
//  Options: --stm32 4100 --esp32 4200
// ============================================================================
import net from 'node:net';
import { TelnetFilter } from '../src/frameParser.js';

const arg = (k, d) => { const i = process.argv.indexOf(`--${k}`); return i > 0 ? process.argv[i + 1] : d; };
const PORTS = { STM32: Number(arg('stm32', 4100)), ESP32: Number(arg('esp32', 4200)) };

const ends = {};
const stats = { 'STM32→ESP32': 0, 'ESP32→STM32': 0 };

// Telnet: byte 255 in the data must be doubled (IAC IAC)
const escapeIac = (s) => Buffer.from(s, 'latin1').toString('latin1').replace(/\xff/g, '\xff\xff');

function connect(name) {
  const other = name === 'STM32' ? 'ESP32' : 'STM32';
  const telnet = new TelnetFilter();
  const sock = net.createConnection({ host: 'localhost', port: PORTS[name] });
  let line = '';

  sock.on('connect', () => {
    ends[name] = sock;
    console.log(`[CABLE] ${name} connected (port ${PORTS[name]})${ends[other] ? ' — cable plugged in on both ends ✅' : ''}`);
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
    // Print the text messages ("# ...") from both boards
    for (const ch of text) {
      if (ch === '\n') { if (line.startsWith('#')) console.log(`[${name}] ${line.trim()}`); line = ''; }
      else if (ch !== '\r') line += ch;
    }
  });

  sock.on('error', (e) => {
    if (e.code === 'ECONNREFUSED') console.log(`[CABLE] ${name} unreachable (port ${PORTS[name]}) — is the simulation running and visible?`);
    else console.log(`[CABLE] ${name}: ${e.message}`);
  });

  sock.on('close', () => {
    if (ends[name] === sock) delete ends[name];
    setTimeout(() => connect(name), 2000);       // automatic reconnection
  });
}

connect('STM32');
connect('ESP32');

setInterval(() => {
  const s = Object.entries(stats).map(([k, v]) => `${k}: ${v} B`).join('  |  ');
  process.stdout.write(`\r[CABLE] ${s}   `);
}, 2000);
