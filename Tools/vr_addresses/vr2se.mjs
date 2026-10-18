// A SkyrimVR address back to its SkyrimSE 1.6.1170 counterpart.
//
// Everything else here runs SE -> VR, because that is the direction the client needs. A crash dump
// runs the other way: the stack is full of VR addresses and the question is what the same function
// looks like on the side where the answer is known. The chain is the same one resolve.mjs uses,
// inverted:
//
//   VR addr --sse_vr.csv--> 1.5.97 addr --offsets-1-5-97-0.csv--> sse id --se_ae.csv--> ae id
//           --versionlib-1-6-1170--> SE addr
//
// database.csv is consulted first, since a human-verified row there also carries a name.
//
//   node Tools/vr_addresses/vr2se.mjs 0x1403ad660 0x1405cf480

import fs from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';
import { loadVersionLib } from './versionlib.mjs';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const CACHE = path.join(HERE, 'cache');
const SE_GAME = 'C:/Program Files (x86)/Steam/steamapps/common/Skyrim Special Edition';

const addrs = process.argv.slice(2);
if (!addrs.length) {
  console.error('usage: vr2se.mjs <vr address> [more...]');
  process.exit(2);
}

const lines = f => fs.readFileSync(path.join(CACHE, f), 'latin1').split(/\r?\n/);
const hex = s => parseInt(String(s).trim().replace(/^0x/i, ''), 16);

// curated, with names
const curated = new Map();
for (const line of lines('database.csv').slice(1)) {
  const c = line.split(',');
  if (c.length < 5) continue;
  curated.set(hex(c[2]), { sse: hex(c[1]), status: c[3], name: c.slice(4).join(',').trim() });
}

// the automated diff, VR -> 1.5.97
const vrToSse = new Map();
for (const line of lines('sse_vr.csv').slice(1)) {
  const c = line.split(',');
  if (c.length < 2) continue;
  vrToSse.set(hex(c[1]), hex(c[0]));
}

// 1.5.97 address -> sse id
const sseAddrToId = new Map();
for (const line of lines('offsets-1-5-97-0.csv').slice(1)) {
  const c = line.split(',');
  if (c.length < 2) continue;
  sseAddrToId.set(hex(c[1]), Number(c[0])); // already absolute in this file, no image base to add
}

// sse id -> ae id. Only ~30k ids carry through this way, so the address match below is the
// fallback that covers the rest.
const sseToAe = new Map();
for (const line of lines('se_ae.csv').slice(1)) {
  const c = line.split(',');
  if (c.length < 2) continue;
  sseToAe.set(Number(c[0]), { ae: Number(c[1]), name: c.slice(3).join(',').trim() });
}

// 1.5.97 address -> 1.6.318 address, and 1.6.318 address -> ae id
const sseAddrToAeAddr = new Map();
for (const line of lines('se-ae-attempted-match.csv').slice(1)) {
  const c = line.split(',');
  if (c.length < 2) continue;
  sseAddrToAeAddr.set(hex(c[0]), hex(c[1]));
}
const aeAddrToId = new Map();
for (const line of lines('offsets-1-6-318-0.csv').slice(1)) {
  const c = line.split(',');
  if (c.length < 2) continue;
  aeAddrToId.set(hex(c[1]), Number(c[0]));
}

const lib = loadVersionLib(path.join(SE_GAME, 'Data/SKSE/Plugins/versionlib-1-6-1170-0.bin'));
const hx = v => (v === undefined || v === null ? '?' : '0x' + v.toString(16));

for (const a of addrs) {
  const vr = Number(a);
  const c = curated.get(vr);
  const sse = c ? c.sse : vrToSse.get(vr);
  const src = c ? `database.csv status ${c.status}` : sse ? 'sse_vr.csv' : null;
  if (!sse) { console.log(`${a}  no 1.5.97 counterpart`); continue; }

  const sseId = sseAddrToId.get(sse);
  let ae = sseId === undefined ? undefined : sseToAe.get(sseId);
  let route = ae ? 'id map' : '';
  if (!ae) {
    const aeAddr = sseAddrToAeAddr.get(sse);
    const aeId = aeAddr === undefined ? undefined : aeAddrToId.get(aeAddr);
    if (aeId !== undefined) { ae = { ae: aeId, name: '' }; route = 'addr match'; }
  }
  const seOff = ae ? lib.offsets.get(ae.ae) : undefined;

  const name = (c && c.name) || (ae && ae.name) || '';
  console.log(
    `${a}  ->  1.5.97 ${hx(sse)}  sseid ${sseId ?? '?'}  aeid ${ae ? ae.ae : '?'}  ` +
    `SE 1.6.1170 ${seOff === undefined ? "?" : hx(seOff + 0x140000000)}  [${src}${route ? ", " + route : ""}]${name ? '  ' + name : ''}`);
}
