// An address to its enclosing function, and to whatever the address layer already knows about it.
//
// Every other tool here answers "where is this id", and the questions that come up while reading a
// disassembly are the other way round: an xref, a return address in a dump, a call target. This
// turns such an address into a function start, and then into an id and a name if the client uses
// it.
//
// .pdata is the function table, but an MSVC function can own several entries. An entry whose
// unwind info carries UNW_FLAG_CHAININFO is a fragment and its parent is another entry, so the
// chain is followed to the real start. That is why `end - begin` is not printed: for a fragment it
// is not the function's length.
//
// Usage:  node Tools/vr_addresses/fn.mjs vr 0x140547ce3 [more addresses...]
//         node Tools/vr_addresses/fn.mjs se 0x140625665

import fs from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';
import { openImage, SE_PATH, VR_PATH } from './pe.mjs';
import { loadVersionLib } from './versionlib.mjs';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const SE_GAME = 'C:/Program Files (x86)/Steam/steamapps/common/Skyrim Special Edition';

const [which, ...addrs] = process.argv.slice(2);
if (!which || !addrs.length) {
  console.error('usage: fn.mjs <se|vr> <address> [address...]');
  process.exit(2);
}

const img = openImage(which === 'se' ? SE_PATH : VR_PATH);
const abs = rva => '0x' + (rva + img.imageBase).toString(16);

// ---- .pdata, sorted, with chained fragments folded into their parent
const pdata = img.sections.find(s => s.name === '.pdata');
const entries = [];
for (let o = pdata.rawOffset; o < pdata.rawOffset + Math.min(pdata.rawSize, pdata.virtualSize); o += 12) {
  const begin = img.buf.readUInt32LE(o);
  if (!begin) break;
  entries.push({ begin, end: img.buf.readUInt32LE(o + 4), unwind: img.buf.readUInt32LE(o + 8) });
}
entries.sort((a, b) => a.begin - b.begin);

const byBegin = new Map(entries.map(e => [e.begin, e]));

// UNW_FLAG_CHAININFO: the entry is a fragment and the RUNTIME_FUNCTION of its parent follows the
// unwind codes. Walk up until an entry that owns itself.
function root(e) {
  for (let guard = 0; guard < 16; guard++) {
    const o = img.toOffset(e.unwind);
    if (o < 0) return e;
    const flags = img.buf[o] >> 3;
    if (!(flags & 0x4)) return e;
    const codes = img.buf[o + 2];
    const parent = o + 4 + (codes + (codes & 1)) * 2;
    const begin = img.buf.readUInt32LE(parent);
    const up = byBegin.get(begin) || { begin, end: img.buf.readUInt32LE(parent + 4), unwind: img.buf.readUInt32LE(parent + 8) };
    if (up.begin === e.begin) return e;
    e = up;
  }
  return e;
}

function enclosing(rva) {
  let lo = 0, hi = entries.length - 1, hit = null;
  while (lo <= hi) {
    const mid = (lo + hi) >> 1;
    if (rva < entries[mid].begin) hi = mid - 1;
    else if (rva >= entries[mid].end) lo = mid + 1;
    else { hit = entries[mid]; break; }
  }
  return hit && root(hit);
}

// ---- what the address layer knows
const rows = fs.readFileSync(path.join(HERE, 'report.tsv'), 'latin1').split(/\r?\n/).slice(1);
const known = new Map(); // rva -> "id name (confidence) site"
if (which === 'vr') {
  for (const line of rows) {
    const c = line.split('\t');
    if (c.length < 5 || !c[4] || !c[4].startsWith('0x')) continue;
    known.set(Number(c[4]) - 0x140000000, `id ${c[0]} ${c[2]}  ${c[3]}  ${c[10] || ''}`);
  }
} else {
  const lib = loadVersionLib(path.join(SE_GAME, 'Data/SKSE/Plugins/versionlib-1-6-1170-0.bin'));
  const name = new Map();
  for (const line of rows) {
    const c = line.split('\t');
    if (c.length > 2) name.set(Number(c[0]), `${c[2]}  ${c[10] || ''}`);
  }
  for (const [id, off] of lib.offsets) known.set(off, `id ${id}${name.has(id) ? '  ' + name.get(id) : ''}`);
}

for (const a of addrs) {
  const rva = img.norm(Number(a));
  const f = enclosing(rva);
  if (!f) { console.log(`${a}  not in .pdata`); continue; }
  const tag = known.get(f.begin);
  console.log(`${a}  in ${abs(f.begin)}+0x${(rva - f.begin).toString(16)}${tag ? '   <<< ' + tag : ''}`);
}
