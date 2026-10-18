// Proposes candidates for AE ids the automatic chain could not map.
//
// Address Library ids are handed out in increasing address order, so the ids surrounding an
// unmapped one bracket where it has to live. Taking the nearest mapped neighbours on each side
// gives a window in SSE 1.5.97 address space, and the curated VR database entries inside that
// window are the candidates, with their names as the deciding evidence.
//
// Usage:  node Tools/vr_addresses/candidates.mjs [aeid ...]
//         with no arguments, every unmapped id in report.tsv is processed

import fs from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';
import { openImage, VR_PATH } from './pe.mjs';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const CACHE = path.join(HERE, 'cache');
const hex = s => parseInt(String(s).trim().replace(/^0x/i, ''), 16);

function forEachLine(file, cb) {
  const text = fs.readFileSync(path.join(CACHE, file), 'latin1');
  let i = text.indexOf('\n') + 1;
  while (i < text.length) {
    let nl = text.indexOf('\n', i);
    if (nl < 0) nl = text.length;
    let end = nl;
    if (end > i && text.charCodeAt(end - 1) === 13) end--;
    if (end > i) cb(text.slice(i, end));
    i = nl + 1;
  }
}

// ---- which ids to work on
const rows = (() => {
  const lines = fs.readFileSync(path.join(HERE, 'report.tsv'), 'utf8').trim().split('\n');
  const cols = lines[0].split('\t');
  return lines.slice(1).map(l => Object.fromEntries(l.split('\t').map((v, i) => [cols[i], v])));
})();

const argIds = process.argv.slice(2).filter(a => /^\d+$/.test(a)).map(Number);
const targets = argIds.length ? rows.filter(r => argIds.includes(+r.aeid)) : rows.filter(r => !r.vr_offset);
if (!targets.length) {
  console.log('nothing to do');
  process.exit(0);
}

const WINDOW = 400; // AE ids to look at on each side
const wantIds = new Set();
for (const t of targets) for (let d = -WINDOW; d <= WINDOW; d++) wantIds.add(+t.aeid + d);

// ---- AE id -> AE address, for the target and its neighbourhood
const aeAddr = new Map();
forEachLine('offsets-1-6-318-0.csv', l => {
  const c = l.indexOf(',');
  const id = +l.slice(0, c);
  if (wantIds.has(id)) aeAddr.set(id, hex(l.slice(c + 1)));
});

// ---- AE address -> 1.5.97 address for those
const wantAe = new Set(aeAddr.values());
const aeToSse = new Map();
forEachLine('se-ae-attempted-match.csv', l => {
  const c = l.indexOf(',');
  const ae = hex(l.slice(c + 1));
  if (wantAe.has(ae) && !aeToSse.has(ae)) aeToSse.set(ae, hex(l.slice(0, c)));
});

// ---- the whole curated VR database, sorted by 1.5.97 address
const curated = [];
for (const l of fs.readFileSync(path.join(CACHE, 'database.csv'), 'utf8').split('\n').slice(1)) {
  if (!l.trim()) continue;
  const f = l.replace(/\r$/, '').split(',');
  const sse = hex(f[1]), vr = hex(f[2]);
  if (!sse || !vr) continue;
  curated.push({ sseId: +f[0], sse, vr, status: +f[3] || 0, name: (f.slice(4).join(',') || '').replace(/^"|"$/g, '').trim() });
}
curated.sort((a, b) => a.sse - b.sse);

const img = openImage(VR_PATH);
const pdata = (() => {
  const sec = img.sections.find(s => s.name === '.pdata');
  const n = Math.floor(Math.min(sec.virtualSize, sec.rawSize) / 12);
  const starts = new Set();
  for (let i = 0; i < n; i++) starts.add(img.buf.readUInt32LE(sec.rawOffset + i * 12));
  return starts;
})();

// ---- report
for (const t of targets) {
  const id = +t.aeid;
  const ae = aeAddr.get(id);
  console.log(`\n=== ${id}  ${t.kind}  ${t.name}   (${t.site})`);
  console.log(`    AE 1.6.318 address: ${ae ? '0x' + ae.toString(16) : 'unknown'}`);

  // nearest mapped neighbours by id, in 1.5.97 space
  let lo = null, hi = null;
  for (let d = 1; d <= WINDOW && (!lo || !hi); d++) {
    if (!lo) { const s = aeToSse.get(aeAddr.get(id - d)); if (s) lo = { id: id - d, sse: s }; }
    if (!hi) { const s = aeToSse.get(aeAddr.get(id + d)); if (s) hi = { id: id + d, sse: s }; }
  }
  if (!lo || !hi) {
    console.log('    could not bracket it (no mapped neighbours within the window)');
    continue;
  }
  const [from, to] = lo.sse < hi.sse ? [lo.sse, hi.sse] : [hi.sse, lo.sse];
  console.log(`    bracketed by AE ${lo.id} and ${hi.id} -> 1.5.97 window 0x${from.toString(16)} .. 0x${to.toString(16)}`);

  const inWindow = curated.filter(c => c.sse > from && c.sse < to);
  if (!inWindow.length) {
    console.log('    no curated VR entries inside the window');
    continue;
  }
  console.log(`    ${inWindow.length} curated VR entries in the window:`);
  for (const c of inWindow.slice(0, 14)) {
    const isStart = pdata.has(c.vr - img.imageBase);
    console.log(`      sse 0x${c.sse.toString(16)}  vr 0x${c.vr.toString(16)}  status ${c.status}  ${isStart ? 'fn-start' : 'no-pdata'}  ${c.name.slice(0, 70)}`);
  }
  if (inWindow.length > 14) console.log(`      ... ${inWindow.length - 14} more`);
}
