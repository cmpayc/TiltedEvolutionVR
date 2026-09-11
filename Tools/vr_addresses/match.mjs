// Searches SkyrimVR.exe for the function behind an AE id, by comparing against the same
// function in the installed SkyrimSE build.
//
// Note on what this can and cannot do. Anniversary Edition was recompiled from a later source
// revision, so its bytes differ from VR 1.4.15 even where a mapping is correct: Renderer::Init
// is 2562 bytes in SE 1.6.1170 and 519 in VR, and that mapping is human verified as correct.
// So a poor score proves nothing and this is NOT a verifier. What it does do is give strong
// positive evidence: when a VR function shares a long identical prologue with the SE function
// AND matches its length exactly, and no other VR function does, that is a real identification.
//
// Usage:  node Tools/vr_addresses/match.mjs search <aeid> [...]
//         with no ids, every unmapped id in report.tsv is searched

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

const se = openImage(SE_PATH);
const vr = openImage(VR_PATH);

// ---- versionlib for the installed SE build
const libs = fs.readdirSync(path.join(SE_GAME, 'Data/SKSE/Plugins')).filter(f => /^versionlib-.*\.bin$/.test(f));
const seVersion = (() => {
  // match the exe's own version so the offsets line up with the bytes we read
  const v = '1-6-1170-0';
  const exact = libs.find(f => f === `versionlib-${v}.bin`);
  return exact || libs.sort().at(-1);
})();
const lib = loadVersionLib(path.join(SE_GAME, 'Data/SKSE/Plugins', seVersion));
console.log(`SE ${lib.version} (${seVersion}): ${lib.offsets.size} ids`);
console.log(`SE image ${se.path}`);
console.log(`VR image ${vr.path}\n`);

// ---- .pdata function tables
function functionTable(img) {
  const sec = img.sections.find(s => s.name === '.pdata');
  const n = Math.floor(Math.min(sec.virtualSize, sec.rawSize) / 12);
  const entries = [];
  for (let i = 0; i < n; i++) {
    const o = sec.rawOffset + i * 12;
    entries.push([img.buf.readUInt32LE(o), img.buf.readUInt32LE(o + 4)]);
  }
  entries.sort((a, b) => a[0] - b[0]);
  const starts = new Int32Array(entries.map(e => e[0]));
  const ends = new Int32Array(entries.map(e => e[1]));
  const lenOf = new Map();
  for (let i = 0; i < starts.length; i++) if (!lenOf.has(starts[i])) lenOf.set(starts[i], ends[i] - starts[i]);
  return { starts, ends, lenOf };
}
const seFns = functionTable(se);
const vrFns = functionTable(vr);

// ---- scoring
function prologueMatch(aBytes, bBytes) {
  if (!aBytes || !bBytes) return -1;
  const n = Math.min(aBytes.length, bBytes.length);
  let i = 0;
  while (i < n && aBytes[i] === bBytes[i]) i++;
  return i;
}

function score(seRva, vrRva, window = 64) {
  const a = se.read(seRva, window);
  const b = vr.read(vrRva, window);
  const pro = prologueMatch(a, b);
  const seLen = seFns.lenOf.get(seRva);
  const vrLen = vrFns.lenOf.get(vrRva);
  const lenDelta = seLen !== undefined && vrLen !== undefined ? vrLen - seLen : null;
  return { pro, seLen, vrLen, lenDelta };
}

// ---- report rows
const lines = fs.readFileSync(path.join(HERE, 'report.tsv'), 'utf8').trim().split('\n');
const cols = lines[0].split('\t');
const rows = lines.slice(1).map(l => Object.fromEntries(l.split('\t').map((v, i) => [cols[i], v])));

const mode = process.argv[2] || 'search';

if (mode === 'search') {
  const ids = process.argv.slice(3).filter(a => /^\d+$/.test(a)).map(Number);
  const targets = ids.length ? rows.filter(r => ids.includes(+r.aeid)) : rows.filter(r => !r.vr_offset);

  for (const r of targets) {
    const seOff = lib.offsets.get(+r.aeid);
    console.log(`\n=== ${r.aeid}  ${r.kind}  ${r.name}   (${r.site})`);
    if (seOff === undefined) { console.log('  not in the installed SE versionlib'); continue; }
    const seLen = seFns.lenOf.get(seOff);
    const seBytes = se.read(seOff, 32);
    console.log(`  SE 1.6.1170 0x${(seOff + se.imageBase).toString(16)}  len ${seLen ?? '?'}  ${seBytes ? [...seBytes.subarray(0, 16)].map(b => b.toString(16).padStart(2, '0')).join(' ') : ''}`);
    if (!se.isExecutable(seOff)) { console.log('  SE address is data, byte search does not apply'); continue; }

    // score every VR function; a real match has a long identical prologue
    const hits = [];
    for (let i = 0; i < vrFns.starts.length; i++) {
      const cand = vrFns.starts[i];
      const b = vr.read(cand, 32);
      if (!b) continue;
      let k = 0;
      while (k < 32 && k < seBytes.length && seBytes[k] === b[k]) k++;
      if (k >= 10) hits.push({ cand, pro: k, len: vrFns.lenOf.get(cand) });
    }
    hits.sort((a, b) => b.pro - a.pro || Math.abs((a.len ?? 0) - (seLen ?? 0)) - Math.abs((b.len ?? 0) - (seLen ?? 0)));
    const exactLen = hits.filter(h => h.len === seLen);
    console.log(`  ${hits.length} VR functions share >=10 prologue bytes, ${exactLen.length} of them also match length exactly`);
    for (const h of (exactLen.length ? exactLen : hits).slice(0, 8))
      console.log(`    0x${(h.cand + vr.imageBase).toString(16)}  prologue ${h.pro}B  len ${h.len}${h.len === seLen ? ' (exact)' : ''}`);
    if (exactLen.length === 1) console.log(`  => unique on prologue + exact length: 0x${(exactLen[0].cand + vr.imageBase).toString(16)}`);
  }
  process.exit(0);
}

console.error(`unknown mode ${mode}`);
process.exit(2);
