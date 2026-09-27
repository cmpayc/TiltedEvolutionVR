// Identifies a SkyrimVR function by who it calls, for the ids the mapping chain cannot reach.
//
// Codegen changed between AE and VR, so bytes and lengths do not survive the recompile and
// prologue search is useless for anything common. The call graph does survive: a function that
// called Foo, Bar and Baz in VR still calls them in AE. So take the target's direct callees in
// SE 1.6.1170, map each one to VR through the ordinary chain, and look for the VR function that
// calls that same set. A few dozen independent votes is far more discriminating than a prologue.
// The target's callers are used the same way as a second, independent signal.
//
// The chain used to build the global SE -> VR map is only ~99.9% accurate, which is fine here:
// a handful of bad edges cannot outvote a few dozen good ones. What matters is that the answer
// is checked, which --selftest does by running the matcher against the addresses that are
// already known.
//
// Usage:  node Tools/vr_addresses/callgraph.mjs <aeid> [...]   identify these ids
//         node Tools/vr_addresses/callgraph.mjs                every unmapped id in report.tsv
//         node Tools/vr_addresses/callgraph.mjs --selftest [n] measure accuracy on known ids

import fs from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';
import { openImage, SE_PATH, VR_PATH, HAVE_UNPACKED } from './pe.mjs';
import { loadVersionLib } from './versionlib.mjs';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const CACHE = path.join(HERE, 'cache');
const SE_GAME = 'C:/Program Files (x86)/Steam/steamapps/common/Skyrim Special Edition';

if (!HAVE_UNPACKED) {
  console.error('needs unpacked binaries in cache/ (see the note in pe.mjs)');
  process.exit(2);
}

const hex = s => parseInt(s.trim().replace(/^0x/i, ''), 16);

function forEachLine(file, cb) {
  const text = fs.readFileSync(file, 'latin1');
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

// ---------------------------------------------------------------- call graph of one image

// A function entry is a direct call target or a .pdata start, in both cases only where MSVC
// alignment padding precedes it. The padding test matters in both directions: .pdata splits some
// functions across several entries and a continuation is not an entry, and a stray 0xE8 inside an
// immediate occasionally decodes to a plausible target in the middle of a real function. Letting
// either through splits a function in two and hands half its callees to a phantom. Each entry
// then runs to the next one.
function buildGraph(img, label) {
  const sec = img.sections.find(s => s.name === '.text');
  const from = sec.rawOffset;
  const to = from + Math.min(sec.rawSize, sec.virtualSize);
  const lo = sec.virtualAddress;
  const hi = sec.virtualAddress + sec.virtualSize;
  const rvaAt = o => lo + (o - from);

  // pass 1: every direct call/jmp edge in .text
  const srcOff = [], dstRva = [];
  for (let o = from; o < to - 5; o++) {
    const op = img.buf[o];
    if (op !== 0xe8 && op !== 0xe9) continue;
    const t = rvaAt(o) + 5 + img.buf.readInt32LE(o + 1);
    if (t < lo || t >= hi) continue;
    srcOff.push(o);
    dstRva.push(op === 0xe8 ? t : -t); // negative marks a jmp, which may be intra-function
  }

  const padded = rva => {
    const prev = img.buf[from + (rva - lo) - 1];
    return prev === 0xcc || prev === 0x90;
  };

  const entries = new Set();
  for (const d of dstRva) if (d > 0 && padded(d)) entries.add(d);

  const pdata = img.sections.find(s => s.name === '.pdata');
  const n = Math.floor(Math.min(pdata.virtualSize, pdata.rawSize) / 12);
  for (let i = 0; i < n; i++) {
    const b = img.buf.readUInt32LE(pdata.rawOffset + i * 12);
    if (b >= lo && b < hi && padded(b)) entries.add(b);
  }

  const starts = Int32Array.from([...entries].sort((a, b) => a - b));
  const index = new Map();
  starts.forEach((s, i) => index.set(s, i));

  // which function contains an rva
  const owner = rva => {
    let l = 0, h = starts.length - 1, f = -1;
    while (l <= h) {
      const m = (l + h) >> 1;
      if (starts[m] <= rva) { f = m; l = m + 1; } else h = m - 1;
    }
    return f;
  };

  // pass 2: edges between functions, dropping jmps that stay inside their own function
  const callees = Array.from({ length: starts.length }, () => new Set());
  const callers = Array.from({ length: starts.length }, () => new Set());
  for (let i = 0; i < srcOff.length; i++) {
    const src = owner(rvaAt(srcOff[i]));
    const dst = index.get(Math.abs(dstRva[i]));
    if (src < 0 || dst === undefined || src === dst) continue;
    callees[src].add(dst);
    callers[dst].add(src);
  }

  const edges = callees.reduce((a, s) => a + s.size, 0);
  console.log(`${label}: ${starts.length} functions, ${edges} call edges`);
  return { starts, index, owner, callees, callers, imageBase: img.imageBase };
}

// ---------------------------------------------------------------- global SE 1.6.1170 -> VR map

// Same chain resolve.mjs uses, but run over every id rather than the ones the client needs, and
// anchored on the versionlib of the installed SE build so the addresses match the bytes we read.
// Both routes to a 1.5.97 address are needed: se_ae.csv only carries 30k ids, while the address
// match carries 274k, and coverage is what decides how many votes a match gets.
function buildAddressMap(lib) {
  const db = f => path.join(CACHE, f);

  const sseOfAe = new Map(); // ae id -> sse id
  forEachLine(db('se_ae.csv'), l => {
    const f = l.split(',');
    if (+f[0] && +f[1]) sseOfAe.set(+f[1], +f[0]);
  });

  const addrOfSse = new Map(); // sse id -> 1.5.97 rva
  forEachLine(db('offsets-1-5-97-0.csv'), l => {
    const c = l.indexOf(',');
    addrOfSse.set(+l.slice(0, c), hex(l.slice(c + 1)));
  });

  const aeAddrOfId = new Map(); // ae id -> 1.6.318 rva
  forEachLine(db('offsets-1-6-318-0.csv'), l => {
    const c = l.indexOf(',');
    aeAddrOfId.set(+l.slice(0, c), hex(l.slice(c + 1)));
  });

  const sseOfAeAddr = new Map(); // 1.6.318 rva -> 1.5.97 rva
  forEachLine(db('se-ae-attempted-match.csv'), l => {
    const c = l.indexOf(',');
    const ae = hex(l.slice(c + 1));
    if (!sseOfAeAddr.has(ae)) sseOfAeAddr.set(ae, hex(l.slice(0, c)));
  });

  const vrOfSseAddr = new Map(); // 1.5.97 rva -> VR rva
  forEachLine(db('sse_vr.csv'), l => {
    const c = l.indexOf(',');
    vrOfSseAddr.set(hex(l.slice(0, c)), hex(l.slice(c + 1)));
  });
  // the curated database wins where it has an opinion
  for (const l of fs.readFileSync(db('database.csv'), 'utf8').split('\n').slice(1)) {
    if (!l.trim()) continue;
    const f = l.split(',');
    const sse = hex(f[1]), vr = hex(f[2]);
    if (sse && vr && (+f[3] || 0) >= 3) vrOfSseAddr.set(sse, vr);
  }

  const seToVr = new Map(); // SE 1.6.1170 rva -> VR rva
  let viaId = 0, viaAddr = 0;
  for (const [aeid, seOff] of lib.offsets) {
    let sseAddr = addrOfSse.get(sseOfAe.get(aeid));
    if (sseAddr !== undefined) viaId++;
    else {
      sseAddr = sseOfAeAddr.get(aeAddrOfId.get(aeid));
      if (sseAddr !== undefined) viaAddr++;
    }
    if (sseAddr === undefined) continue;
    const vr = vrOfSseAddr.get(sseAddr);
    if (vr !== undefined) seToVr.set(seOff, vr - 0x140000000);
  }
  console.log(`global map: ${seToVr.size} of ${lib.offsets.size} SE ids carry through to VR (${viaId} via id map, ${viaAddr} via address match)`);
  return seToVr;
}

// ---------------------------------------------------------------- matching

// Turns a set of SE function indices into the VR function indices they map to.
function project(seIdx, se, vr, seToVr) {
  const out = new Set();
  let mapped = 0;
  for (const i of seIdx) {
    const v = seToVr.get(se.starts[i]);
    if (v === undefined) continue;
    mapped++;
    const j = vr.index.get(v);
    if (j !== undefined) out.add(j);
  }
  return { set: out, mapped };
}

function identify(seRva, se, vr, seToVr) {
  const seIdx = se.index.get(seRva);
  if (seIdx === undefined) return { error: 'SE address is not a known function entry' };

  const wantCallees = project(se.callees[seIdx], se, vr, seToVr);
  const wantCallers = project(se.callers[seIdx], se, vr, seToVr);

  // One vote per expected callee, from every VR function that calls it, and the mirror of that
  // for callers. Votes are weighted by how rare the shared neighbour is: also calling a function
  // that only two others call is near proof, while also calling the allocator says nothing. That
  // weighting is what stops the allocator's neighbours from all looking like each other.
  const score = new Map();
  const bump = (i, field, w) => {
    let s = score.get(i);
    if (!s) score.set(i, (s = { callee: 0, caller: 0, weight: 0 }));
    s[field]++;
    s.weight += w;
  };
  for (const c of wantCallees.set) {
    const w = 1 / Math.max(1, vr.callers[c].size);
    for (const f of vr.callers[c]) bump(f, 'callee', w);
  }
  for (const c of wantCallers.set) {
    const w = 1 / Math.max(1, vr.callees[c].size);
    for (const f of vr.callees[c]) bump(f, 'caller', w);
  }

  const byIdx = new Map([...score].map(([i, s]) => [i, { i, rva: vr.starts[i], ...s, total: s.callee + s.caller, absorbed: [] }]));

  // AE inlined helpers that VR keeps as separate functions, and the helper owns the calls, so it
  // collects the votes its caller deserves. Where a scoring candidate has exactly one caller in
  // the whole image and that caller also scored, the helper is not a rival reading of the target,
  // it is part of it: fold its score upward. Renderer::Init is the case this exists for, where
  // the inlined window setup outscored the real answer seven to one.
  for (let pass = 0; pass < 3; pass++) {
    let folded = false;
    for (const r of [...byIdx.values()].sort((a, b) => a.weight - b.weight)) {
      if (!byIdx.has(r.i) || vr.callers[r.i].size !== 1) continue;
      const p = byIdx.get([...vr.callers[r.i]][0]);
      if (!p || p.i === r.i) continue;
      p.weight += r.weight;
      p.callee += r.callee;
      p.caller += r.caller;
      p.total += r.total;
      p.absorbed.push(r.rva, ...r.absorbed);
      byIdx.delete(r.i);
      folded = true;
    }
    if (!folded) break;
  }

  const ranked = [...byIdx.values()].sort((a, b) => b.weight - a.weight);

  return {
    seIdx,
    expectCallees: se.callees[seIdx].size,
    expectCallers: se.callers[seIdx].size,
    projCallees: wantCallees.set.size,
    projCallers: wantCallers.set.size,
    ranked,
  };
}

const abs = rva => '0x' + (rva + 0x140000000).toString(16).toUpperCase();

// ---------------------------------------------------------------- run

const lib = loadVersionLib(path.join(SE_GAME, 'Data/SKSE/Plugins/versionlib-1-6-1170-0.bin'));
console.log(`SE versionlib ${lib.version}, ${lib.offsets.size} ids`);
const se = buildGraph(openImage(SE_PATH), 'SE 1.6.1170');
const vr = buildGraph(openImage(VR_PATH), 'VR 1.4.15  ');
const seToVr = buildAddressMap(lib);

const lines = fs.readFileSync(path.join(HERE, 'report.tsv'), 'utf8').trim().split('\n');
const cols = lines[0].split('\t');
const rows = lines.slice(1).map(l => Object.fromEntries(l.split('\t').map((v, i) => [cols[i], v])));

// A result is only worth acting on when the winner is clear of the runner-up. Ties mean the
// evidence does not distinguish them and the answer is unknown, not "probably the first one".
// Above STRONG the self-test has never been wrong; below it, one answer in nine was. Nothing here
// goes into overrides.csv without being confirmed against the binary either way, but the band
// says how hard to look.
const STRONG = 2.0;
const decide = r => {
  const [a, b] = r.ranked;
  if (!a || a.weight < 1 || a.total < 4) return null;
  if (b && a.weight < 2 * b.weight) return null;
  return a;
};

if (process.argv[2] === '--selftest') {
  // Every mapped code address whose confidence comes from the curated database is ground truth
  // for the matcher. Anything it gets wrong there it would also get wrong on a real target.
  const limit = Number(process.argv[3]) || Infinity;
  const truth = rows.filter(r => r.kind !== 'rtti' && r.vr_offset && (r.confidence === 'override' || r.confidence.startsWith('curated')));
  let tested = 0, abstained = 0, noSe = 0;
  const correct = [], wrong = [];

  for (const r of truth) {
    if (tested >= limit) break;
    const seOff = lib.offsets.get(+r.aeid);
    if (seOff === undefined || se.index.get(seOff) === undefined) { noSe++; continue; }
    tested++;
    const res = identify(seOff, se, vr, seToVr);
    const pick = decide(res);
    if (!pick) { abstained++; continue; }
    (pick.rva === parseInt(r.vr_offset, 16) ? correct : wrong).push({ r, pick });
  }

  const band = w => (w >= STRONG ? 'strong' : 'weak');
  const tally = t => ({ strong: t.filter(x => x.pick.weight >= STRONG).length, weak: t.filter(x => x.pick.weight < STRONG).length });
  const ok = tally(correct), bad = tally(wrong);
  const answered = correct.length + wrong.length;

  console.log(`\nselftest over ${tested} known code addresses (${noSe} skipped, not function entries in SE)`);
  console.log(`  answered   ${answered}`);
  console.log(`  correct    ${correct.length}${answered ? `  (${(100 * correct.length / answered).toFixed(1)}% of answers)` : ''}`);
  console.log(`  WRONG      ${wrong.length}`);
  console.log(`  abstained  ${abstained}  (no candidate cleared the margin)`);
  console.log(`\n  by confidence band (weight >= ${STRONG.toFixed(1)} is strong)`);
  console.log(`    strong   ${ok.strong} correct, ${bad.strong} wrong`);
  console.log(`    weak     ${ok.weak} correct, ${bad.weak} wrong`);
  for (const { r, pick } of wrong)
    console.log(`    ${r.aeid.padEnd(7)} ${r.name.padEnd(26)} want ${abs(parseInt(r.vr_offset, 16))} got ${abs(pick.rva)} weight ${pick.weight.toFixed(2)} (${band(pick.weight)})`);
  process.exit(0);
}

const ids = process.argv.slice(2).filter(a => /^\d+$/.test(a)).map(Number);
const targets = ids.length ? rows.filter(r => ids.includes(+r.aeid)) : rows.filter(r => !r.vr_offset && r.kind !== 'rtti');

for (const r of targets) {
  const seOff = lib.offsets.get(+r.aeid);
  console.log(`\n=== ${r.aeid}  ${r.kind}  ${r.name}   (${r.site})`);
  if (seOff === undefined) { console.log('  not in the SE versionlib'); continue; }
  console.log(`  SE 1.6.1170 ${abs(seOff)}`);

  const res = identify(seOff, se, vr, seToVr);
  if (res.error) { console.log(`  ${res.error}`); continue; }
  console.log(`  callees ${res.expectCallees} -> ${res.projCallees} projected into VR, callers ${res.expectCallers} -> ${res.projCallers}`);

  for (const c of res.ranked.slice(0, 5))
    console.log(`    ${abs(c.rva).padEnd(12)} weight ${c.weight.toFixed(2).padStart(7)}  votes ${String(c.total).padStart(4)} (${c.callee} callee + ${c.caller} caller)${c.absorbed.length ? `  absorbed ${c.absorbed.map(abs).join(' ')}` : ''}`);

  const pick = decide(res);
  console.log(pick ? `  => ${abs(pick.rva)}  ${pick.weight >= STRONG ? 'strong' : 'WEAK, treat as a lead only'}` : '  => no confident answer');
}
