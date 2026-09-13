// Verifies the generated VR address table against SkyrimVR.exe itself, rather than trusting
// the mapping databases.
//
// SkyrimVR.exe is wrapped in SteamStub, so .text is encrypted on disk and the instruction
// bytes cannot be compared. Two things are readable and are enough to catch the mistakes that
// matter:
//
//   .data/.rdata  hold the MSVC RTTI type descriptors, each carrying its own mangled class
//                 name. That derives every RTTI address outright instead of trusting a
//                 mapping, covering the large majority of the table.
//   .pdata        is the exception unwind table, one entry per non-leaf function with its
//                 exact start and end. An address that should be a function start either is
//                 one or is not, and an address landing inside a different function is
//                 definitely wrong.
//
// Usage:  node Tools/vr_addresses/verify.mjs [--all]
//   --all  list every checked entry, not just the problems

import fs from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';
import { openImage, VR_PATH } from './pe.mjs';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const REPO = path.resolve(HERE, '..', '..');
const REPORT = path.join(HERE, 'report.tsv');
const verbose = process.argv.includes('--all');

const img = openImage(VR_PATH);

// ---------------------------------------------------------------- RTTI type descriptors
// A TypeDescriptor is { void* pVFTable; void* spare; char name[]; }, so the descriptor starts
// 16 bytes before its name.
function buildRttiIndex() {
  const bySimpleName = new Map(); // simple class name -> [addresses]
  const needle = Buffer.from('.?AV', 'latin1');

  for (const sec of img.sections) {
    if (!sec.rawSize || sec.name === '.text' || sec.name === '.bind') continue;
    const start = sec.rawOffset;
    const end = Math.min(sec.rawOffset + sec.rawSize, img.buf.length);

    let at = start;
    while ((at = img.buf.indexOf(needle, at)) >= 0 && at < end) {
      let z = at;
      while (z < end && img.buf[z] !== 0) z++;
      const mangled = img.buf.toString('latin1', at, z);
      // ".?AVTESForm@@" -> TESForm,  ".?AVInner@Outer@@" -> Inner
      const simple = mangled.slice(4).split('@')[0];
      if (simple && /^[A-Za-z_?$][\w?$]*$/.test(simple)) {
        const rva = sec.virtualAddress + (at - sec.rawOffset) - 16;
        if (!bySimpleName.has(simple)) bySimpleName.set(simple, []);
        bySimpleName.get(simple).push(rva);
      }
      at = z + 1;
    }
  }
  return bySimpleName;
}

// ---------------------------------------------------------------- .pdata function table
function buildFunctionTable() {
  const sec = img.sections.find(s => s.name === '.pdata');
  if (!sec) throw new Error('no .pdata section');
  const count = Math.floor(Math.min(sec.virtualSize, sec.rawSize) / 12);
  const begin = new Int32Array(count);
  const end = new Int32Array(count);
  for (let i = 0; i < count; i++) {
    const o = sec.rawOffset + i * 12;
    begin[i] = img.buf.readUInt32LE(o);
    end[i] = img.buf.readUInt32LE(o + 4);
  }
  const order = [...begin.keys()].sort((a, b) => begin[a] - begin[b]);
  const b = Int32Array.from(order, i => begin[i]);
  const e = Int32Array.from(order, i => end[i]);

  // returns {exact} | {inside, start, end} | null
  const classify = rva => {
    let lo = 0, hi = b.length - 1, found = -1;
    while (lo <= hi) {
      const mid = (lo + hi) >> 1;
      if (b[mid] <= rva) { found = mid; lo = mid + 1; } else hi = mid - 1;
    }
    if (found < 0) return null;
    if (b[found] === rva) return { exact: true, start: b[found], end: e[found] };
    if (rva < e[found]) return { exact: false, inside: true, start: b[found], end: e[found] };
    return null;
  };
  return { count: b.length, classify };
}

// ---------------------------------------------------------------- load the report
if (!fs.existsSync(REPORT)) {
  console.error('report.tsv missing, run resolve.mjs first');
  process.exit(2);
}
const lines = fs.readFileSync(REPORT, 'utf8').trim().split('\n');
const cols = lines[0].split('\t');
const rows = lines.slice(1).map(l => Object.fromEntries(l.split('\t').map((v, i) => [cols[i], v])));

const rtti = buildRttiIndex();
const fns = buildFunctionTable();
console.log(`SkyrimVR.exe: ${rtti.size} distinct RTTI class names, ${fns.count} functions in .pdata\n`);

// ---------------------------------------------------------------- check
const result = { rttiOk: 0, rttiWrong: [], rttiAmbiguous: [], rttiAbsent: [], fnExact: 0, fnInside: [], fnUnknown: [], notPadded: [], data: 0, unmapped: [] };

for (const r of rows) {
  const rva = r.vr_offset ? parseInt(r.vr_offset, 16) : 0;
  if (!rva) { result.unmapped.push(r); continue; }

  if (r.kind === 'rtti') {
    const cands = rtti.get(r.name);
    if (!cands) result.rttiAbsent.push(r);
    else if (cands.includes(rva)) { result.rttiOk++; if (cands.length > 1) result.rttiAmbiguous.push({ r, cands }); }
    else result.rttiWrong.push({ r, rva, cands });
    continue;
  }

  if (!img.isExecutable(rva)) { result.data++; continue; }

  // Padding before the address is independent evidence of a function entry, and it needs
  // decrypted .text. MSVC splits some functions across several .pdata entries, so a .pdata
  // BeginAddress alone is not proof of an entry point.
  const prev = img.read(rva - 1, 1);
  const padded = prev ? (prev[0] === 0xcc || prev[0] === 0x90 || prev[0] === 0xc3 || prev[0] === 0x00) : null;
  if (padded === false) result.notPadded.push({ r, rva, prev: prev[0] });

  const c = fns.classify(rva);
  if (c && c.exact) result.fnExact++;
  else if (c && c.inside) result.fnInside.push({ r, rva, c });
  else result.fnUnknown.push({ r, rva });
}

// ---------------------------------------------------------------- report
console.log('RTTI type descriptors (derived from the binary, authoritative)');
console.log(`  confirmed exactly       ${result.rttiOk}`);
console.log(`  WRONG address           ${result.rttiWrong.length}`);
console.log(`  class name not in VR    ${result.rttiAbsent.length}`);
console.log(`  name is ambiguous       ${result.rttiAmbiguous.length}`);

console.log('\nCode addresses (checked against the .pdata function table)');
console.log(`  exact function start    ${result.fnExact}`);
console.log(`  INSIDE another function  ${result.fnInside.length}`);
console.log(`  not in .pdata (leaf?)   ${result.fnUnknown.length}`);
console.log(`  NOT preceded by padding ${result.notPadded.length}`);
console.log(`\nData addresses (not code, boundary check not applicable)  ${result.data}`);
console.log(`Unmapped ids                                              ${result.unmapped.length}`);

if (result.rttiWrong.length) {
  console.log('\n--- RTTI addresses contradicted by the binary ---');
  for (const { r, rva, cands } of result.rttiWrong)
    console.log(`  ${r.aeid.padEnd(7)} ${r.name.padEnd(30)} table 0x${rva.toString(16)}  binary ${cands.map(c => '0x' + c.toString(16)).join(', ')}  [${r.confidence}]`);
}
if (result.rttiAbsent.length) {
  console.log('\n--- RTTI class names absent from SkyrimVR.exe (feature may not exist in VR) ---');
  for (const r of result.rttiAbsent) console.log(`  ${r.aeid.padEnd(7)} ${r.name.padEnd(30)} table 0x${r.vr_offset} [${r.confidence}]`);
}
if (result.notPadded.length) {
  console.log('\n--- code addresses with no padding before them, so probably not a function entry ---');
  for (const { r, rva, prev } of result.notPadded)
    console.log(`  ${r.aeid.padEnd(7)} ${r.kind.padEnd(8)} ${r.name.padEnd(28)} 0x${rva.toString(16)}  preceding byte 0x${prev.toString(16)}  [${r.confidence}]  ${r.site}`);
}
if (result.fnInside.length) {
  console.log('\n--- code addresses landing inside a function rather than at its start ---');
  for (const { r, rva, c } of result.fnInside)
    console.log(`  ${r.aeid.padEnd(7)} ${r.kind.padEnd(8)} ${r.name.padEnd(28)} 0x${rva.toString(16)} is +0x${(rva - c.start).toString(16)} into 0x${c.start.toString(16)}..0x${c.end.toString(16)}  [${r.confidence}]`);
}
if (result.fnUnknown.length) {
  console.log('\n--- code addresses with no .pdata entry (leaf function, or wrong) ---');
  for (const { r, rva } of result.fnUnknown) console.log(`  ${r.aeid.padEnd(7)} ${r.kind.padEnd(8)} ${r.name.padEnd(28)} 0x${rva.toString(16)}  [${r.confidence}]`);
}
if (verbose && result.rttiAmbiguous.length) {
  console.log('\n--- RTTI names appearing more than once (confirmed, but worth knowing) ---');
  for (const { r, cands } of result.rttiAmbiguous) console.log(`  ${r.aeid.padEnd(7)} ${r.name.padEnd(30)} ${cands.map(c => '0x' + c.toString(16)).join(', ')}`);
}

// An unmapped RTTI id can be derived outright: the class name in the binary is the answer.
const derived = [];
for (const r of result.unmapped) {
  if (r.kind !== 'rtti') continue;
  const cands = rtti.get(r.name);
  if (cands && cands.length === 1) derived.push({ r, rva: cands[0] });
  else if (cands) derived.push({ r, rva: cands[0], ambiguous: cands });
}
if (derived.length) {
  console.log('\n--- unmapped RTTI ids derived from the binary, paste into overrides.csv ---');
  for (const { r, rva, ambiguous } of derived) {
    const note = ambiguous ? `${r.name} RTTI type descriptor - AMBIGUOUS, ${ambiguous.length} candidates in SkyrimVR.exe` : `${r.name} RTTI type descriptor, located by mangled name in SkyrimVR.exe`;
    console.log(`${r.aeid}, 0x${(rva + img.imageBase).toString(16).toUpperCase()}, ${note}`);
  }
}
const stillMissing = result.unmapped.filter(r => !derived.some(d => d.r.aeid === r.aeid));
if (stillMissing.length) {
  console.log(`\n--- still unmapped, need manual work (${stillMissing.length}) ---`);
  for (const r of stillMissing) console.log(`  ${r.aeid.padEnd(7)} ${r.kind.padEnd(8)} ${r.name.padEnd(28)} ${r.site}`);
}

const hardFailures = result.rttiWrong.length + result.fnInside.length;
console.log(`\n${hardFailures === 0 ? 'no contradictions found' : hardFailures + ' addresses are contradicted by the binary'}`);
