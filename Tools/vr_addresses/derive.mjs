// Derives AE ids the mapping chain missed, by order rather than by guesswork.
//
// Both Address Library databases hand out ids in increasing address order. So if the nearest
// mapped AE ids on either side of an unmapped one are loAe and hiAe, and there are k AE ids
// strictly between them, then those k ids correspond in order to the SSE ids strictly between
// sse(loAe) and sse(hiAe). When the two counts are equal, the correspondence is forced: the
// i-th AE id in the gap is the i-th SSE id in the gap. No name matching needed.
//
// The order assumption is checked, not assumed, and reported before anything else.
//
// Usage:  node Tools/vr_addresses/derive.mjs [aeid ...]

import fs from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';
import { openImage, VR_PATH } from './pe.mjs';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const CACHE = path.join(HERE, 'cache');
const hex = s => parseInt(String(s).trim().replace(/^0x/i, ''), 16);

function readPairs(file) {
  const text = fs.readFileSync(path.join(CACHE, file), 'latin1');
  const ids = [], addrs = [];
  let i = text.indexOf('\n') + 1;
  while (i < text.length) {
    let nl = text.indexOf('\n', i);
    if (nl < 0) nl = text.length;
    let end = nl;
    if (end > i && text.charCodeAt(end - 1) === 13) end--;
    if (end > i) {
      const c = text.indexOf(',', i);
      if (c > i && c < end) {
        const id = +text.slice(i, c);
        const a = hex(text.slice(c + 1, end));
        if (Number.isFinite(id) && Number.isFinite(a)) { ids.push(id); addrs.push(a); }
      }
    }
    i = nl + 1;
  }
  return { ids, addrs };
}

// ---- order check
function orderStats({ ids, addrs }, label) {
  const order = [...ids.keys()].sort((a, b) => ids[a] - ids[b]);
  let bad = 0;
  for (let k = 1; k < order.length; k++) if (addrs[order[k]] < addrs[order[k - 1]]) bad++;
  console.log(`${label}: ${ids.length} entries, ${bad} id-vs-address order violations (${(100 * bad / ids.length).toFixed(3)}%)`);
  const sortedIds = order.map(i => ids[i]);
  const sortedAddrs = order.map(i => addrs[i]);
  return { sortedIds, sortedAddrs };
}

const ae = readPairs('offsets-1-6-318-0.csv');
const sse = readPairs('offsets-1-5-97-0.csv');
const AE = orderStats(ae, 'AE 1.6.318 ');
const SSE = orderStats(sse, 'SSE 1.5.97 ');

const aeAddrOf = new Map();
for (let i = 0; i < AE.sortedIds.length; i++) aeAddrOf.set(AE.sortedIds[i], AE.sortedAddrs[i]);

// ae address -> sse address, from the official attempted match
const aeToSse = new Map();
{
  const text = fs.readFileSync(path.join(CACHE, 'se-ae-attempted-match.csv'), 'latin1');
  let i = text.indexOf('\n') + 1;
  while (i < text.length) {
    let nl = text.indexOf('\n', i);
    if (nl < 0) nl = text.length;
    const c = text.indexOf(',', i);
    if (c > i && c < nl) {
      const s = hex(text.slice(i, c)), a = hex(text.slice(c + 1, nl));
      if (s && a && !aeToSse.has(a)) aeToSse.set(a, s);
    }
    i = nl + 1;
  }
}

// sse address -> VR, curated first then the automated diff
const curatedBySse = new Map();
for (const l of fs.readFileSync(path.join(CACHE, 'database.csv'), 'utf8').split('\n').slice(1)) {
  if (!l.trim()) continue;
  const f = l.replace(/\r$/, '').split(',');
  const s = hex(f[1]), vr = hex(f[2]), st = +f[3] || 0;
  if (!s || !vr) continue;
  const prev = curatedBySse.get(s);
  if (!prev || st > prev.status) curatedBySse.set(s, { vr, status: st, name: (f.slice(4).join(',') || '').replace(/^"|"$/g, '').trim() });
}
const autoBySse = new Map();
{
  const text = fs.readFileSync(path.join(CACHE, 'sse_vr.csv'), 'latin1');
  let i = text.indexOf('\n') + 1;
  while (i < text.length) {
    let nl = text.indexOf('\n', i);
    if (nl < 0) nl = text.length;
    const c = text.indexOf(',', i);
    if (c > i && c < nl) autoBySse.set(hex(text.slice(i, c)), hex(text.slice(c + 1, nl)));
    i = nl + 1;
  }
}

const img = openImage(VR_PATH);
const pdataStarts = (() => {
  const s = img.sections.find(x => x.name === '.pdata');
  const n = Math.floor(Math.min(s.virtualSize, s.rawSize) / 12);
  const set = new Set();
  for (let i = 0; i < n; i++) set.add(img.buf.readUInt32LE(s.rawOffset + i * 12));
  return set;
})();

// ---- targets
const lines = fs.readFileSync(path.join(HERE, 'report.tsv'), 'utf8').trim().split('\n');
const cols = lines[0].split('\t');
const rows = lines.slice(1).map(l => Object.fromEntries(l.split('\t').map((v, i) => [cols[i], v])));
const argIds = process.argv.slice(2).filter(a => /^\d+$/.test(a)).map(Number);
const targets = argIds.length ? rows.filter(r => argIds.includes(+r.aeid)) : rows.filter(r => !r.vr_offset);

const lowerBound = (arr, v) => { let lo = 0, hi = arr.length; while (lo < hi) { const m = (lo + hi) >> 1; if (arr[m] < v) lo = m + 1; else hi = m; } return lo; };

// Returns { ok, reason } or { ok: true, sseAddr, vr, src, loId, hiId, gap }
function deriveOne(id) {
  const aeIdx = lowerBound(AE.sortedIds, id);
  if (AE.sortedIds[aeIdx] !== id) return { reason: 'not in the AE database' };

  let lo = -1, hi = -1;
  for (let k = aeIdx - 1; k >= 0 && lo < 0; k--) if (aeToSse.has(AE.sortedAddrs[k])) lo = k;
  for (let k = aeIdx + 1; k < AE.sortedIds.length && hi < 0; k++) if (aeToSse.has(AE.sortedAddrs[k])) hi = k;
  if (lo < 0 || hi < 0) return { reason: 'cannot bracket' };

  const sseLo = aeToSse.get(AE.sortedAddrs[lo]);
  const sseHi = aeToSse.get(AE.sortedAddrs[hi]);
  if (sseLo >= sseHi) return { reason: 'bracket inverted, order broken here' };

  const gapAe = AE.sortedIds.slice(lo + 1, hi);
  const s0 = lowerBound(SSE.sortedAddrs, sseLo) + 1;
  const s1 = lowerBound(SSE.sortedAddrs, sseHi);
  const gapSse = SSE.sortedAddrs.slice(s0, s1);
  const shape = `AE ${AE.sortedIds[lo]}..${AE.sortedIds[hi]}, gap ${gapAe.length} AE vs ${gapSse.length} SSE`;

  if (gapAe.length !== gapSse.length) return { reason: 'gap sizes differ', shape, gapSse };

  const sseAddr = gapSse[gapAe.indexOf(id)];
  const cur = curatedBySse.get(sseAddr);
  const vr = cur ? cur.vr : autoBySse.get(sseAddr);
  return { ok: true, sseAddr, vr, cur, src: cur ? `curated status ${cur.status}` : (vr ? 'automated diff' : 'none'), shape, gapWidth: gapAe.length };
}

// --selftest measures the order method against the 2806 RTTI ids, whose true VR addresses are
// derived from the mangled class names in the binary and are therefore ground truth.
if (process.argv.includes('--selftest')) {
  const truth = (() => {
    const map = new Map();
    const needle = Buffer.from('.?AV', 'latin1');
    for (const sec of img.sections) {
      if (!sec.rawSize || sec.name === '.text' || sec.name === '.bind') continue;
      const end = Math.min(sec.rawOffset + sec.rawSize, img.buf.length);
      let at = sec.rawOffset;
      while ((at = img.buf.indexOf(needle, at)) >= 0 && at < end) {
        let z = at;
        while (z < end && img.buf[z] !== 0) z++;
        const simple = img.buf.toString('latin1', at, z).slice(4).split('@')[0];
        if (simple && /^[A-Za-z_][\w]*$/.test(simple)) {
          if (!map.has(simple)) map.set(simple, []);
          map.get(simple).push(sec.virtualAddress + (at - sec.rawOffset) - 16 + img.imageBase);
        }
        at = z + 1;
      }
    }
    return map;
  })();

  let correct = 0, wrong = 0, noAnswer = 0, byWidth = new Map();
  const wrongEx = [];
  for (const r of rows.filter(x => x.kind === 'rtti')) {
    const real = truth.get(r.name);
    if (!real || real.length !== 1) continue;
    const d = deriveOne(+r.aeid);
    if (!d.ok || !d.vr) { noAnswer++; continue; }
    const w = d.gapWidth;
    if (!byWidth.has(w)) byWidth.set(w, { c: 0, w: 0 });
    if (d.vr === real[0]) { correct++; byWidth.get(w).c++; }
    else {
      wrong++; byWidth.get(w).w++;
      if (wrongEx.length < 8) wrongEx.push(`  ${r.aeid} ${r.name}: derived 0x${d.vr.toString(16)}, actual 0x${real[0].toString(16)} (gap ${w})`);
    }
  }
  const tested = correct + wrong;
  console.log(`\nSELF TEST of the order method against ${tested} RTTI ids with binary ground truth`);
  console.log(`  correct ${correct}  wrong ${wrong}  ->  ${(100 * correct / tested).toFixed(2)}% accurate`);
  console.log(`  no answer produced ${noAnswer}`);
  console.log('\n  accuracy by gap width (how many ids the bracket had to order):');
  for (const [w, s] of [...byWidth].sort((a, b) => a[0] - b[0]).slice(0, 12)) {
    console.log(`    gap ${String(w).padStart(3)}: ${String(s.c + s.w).padStart(5)} tested, ${(100 * s.c / (s.c + s.w)).toFixed(1)}% correct`);
  }
  if (wrongEx.length) { console.log('\n  examples of wrong derivations:'); for (const e of wrongEx) console.log(e); }
  process.exit(0);
}

console.log(`\nderiving ${targets.length} ids\n${'='.repeat(78)}`);
const overrides = [];

for (const t of targets) {
  const id = +t.aeid;
  console.log(`\n${id}  ${t.kind}  ${t.name}   (${t.site})`);
  const d = deriveOne(id);
  if (d.shape) console.log(`  ${d.shape}`);
  if (!d.ok) {
    console.log(`  ${d.reason}`);
    for (const a of (d.gapSse || []).slice(0, 8)) {
      const c = curatedBySse.get(a);
      if (c) console.log(`    candidate sse 0x${a.toString(16)}  vr 0x${c.vr.toString(16)}  status ${c.status}  ${c.name.slice(0, 62)}`);
    }
    continue;
  }
  console.log(`  order implies AE ${id} = SSE 0x${d.sseAddr.toString(16)}${d.cur ? `  "${d.cur.name.slice(0, 58)}"` : ''}`);
  if (!d.vr) { console.log('  no VR address known for that SSE address'); continue; }

  const rva = d.vr - img.imageBase;
  const kind = img.isExecutable(rva) ? (pdataStarts.has(rva) ? 'exact function start' : 'code, no .pdata entry') : 'data';
  console.log(`  -> VR 0x${d.vr.toString(16)}  (${d.src}, ${kind})`);
  overrides.push(`${id}, 0x${d.vr.toString(16).toUpperCase()}, ${t.name} - order-implied (${d.shape}); SSE 0x${d.sseAddr.toString(16)}; ${d.src}; ${kind}`);
}

if (overrides.length) {
  console.log(`\n${'='.repeat(78)}\ncandidate override lines (order-implied, NOT proven - see --selftest for accuracy):\n`);
  for (const o of overrides) console.log(o);
}
