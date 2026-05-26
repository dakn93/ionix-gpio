/**
 * Minimal Blackmagic-style ATEM UDP simulator on port 9910 (Ethernet protocol).
 * For local tests: enter this machine's LAN IP in the device's ATEM settings.
 *
 * Usage: node atem_udp_sim.mjs [tally_slots]
 */

import dgram from 'node:dgram';

const PORT = 9910;
const tallySlots = Math.min(96, Math.max(8, parseInt(process.argv[2], 10) || 40));
const SESSION_ID = 0x37;

/** First byte: length low 11 bits in (byte0 & 7)<<8 | byte1, upper bits flags; ACK if (byte0 & 0x08). */
function buildHeader(payloadLenIncluding12, byte0Extras = 0x80) {
  const L = payloadLenIncluding12 & 0x7ff;
  const b1 = L & 0xff;
  const b0_hi3 = (L >> 8) & 7;
  const buf = Buffer.alloc(12, 0);
  buf[0] = byte0Extras | b0_hi3;
  buf[1] = b1;
  return buf;
}

function buildTallyPacket(state) {
  /** Segment: [len_hi len_lo] + body, where body is cmdLength-2 bytes (see ATEM::_parsePacket). */
  const body = Buffer.alloc(8 + state.length, 0);
  body.write('TlIn', 2, 4, 'latin1');
  state.copy(body, 8);
  const cmdLength = 2 + body.length;
  const total = 12 + cmdLength;
  const out = Buffer.alloc(total, 0);
  buildHeader(total).copy(out, 0);
  out.writeUInt16BE(cmdLength, 12);
  body.copy(out, 14);
  return out;
}

function buildTerminator12() {
  return buildHeader(12, 0x80);
}

const srv = dgram.createSocket('udp4');

let remote = /** @type {{ addr: string, port: number } | null} */ (null);
let tally = Buffer.alloc(tallySlots, 0);

srv.on('message', (msg, rinfo) => {
  remote = { addr: rinfo.address, port: rinfo.port };
  if (msg.length === 20 && msg[2] === 0x53 && msg[3] === 0xab) {
    const helloRsp = Buffer.from([
      0x10, 0x14, 0x53, 0xab, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3a, 0x00, 0x00,
      0x01, 0x00, 0x00, SESSION_ID, 0x00, 0x00, 0x00, 0x00,
    ]);
    srv.send(helloRsp, rinfo.port, rinfo.address);
    return;
  }
  /** Client second handshake (Arduino ATEM.cpp connectHelloAnswerString). */
  if (msg.length === 12 && msg[0] === 0x80 && msg[2] === 0x53 && msg[3] === 0xab) {
    srv.send(buildTerminator12(), rinfo.port, rinfo.address);
    srv.send(buildTallyPacket(tally), rinfo.port, rinfo.address);
  }
});

function tickDemo() {
  if (!remote) return;
  const t = Math.floor(Date.now() / 1000);
  tally[0] = t % 8 < 4 ? 1 : 2;
  if (tally.length > 1) tally[1] = tally[1] === 0 ? 0 : 2;
  if (tally.length > 2) tally[2] = Math.floor(Date.now() / 500) % 2 ? 1 : 2;
  const pkt = buildTallyPacket(tally);
  srv.send(pkt, remote.port, remote.addr, (err) => {
    if (err) console.error(err.message);
  });
}

srv.on('listening', () => {
  const a = srv.address();
  console.log(
    `[atem-sim] UDP ${a.address}:${a.port} — ${tallySlots} tally slots. Set device ATEM IP to this PC's LAN address.`
  );
  console.log('[atem-sim] Demo: input 1 alternates PGM/PV; input 3 toggles PV.');
});

srv.bind(PORT, () => {
  try {
    srv.setBroadcast(true);
  } catch {}
});

setInterval(tickDemo, 500);
