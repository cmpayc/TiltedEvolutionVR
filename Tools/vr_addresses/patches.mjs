// Checks every byte patch the client applies at a fixed offset inside a resolved function.
//
// Mapping an id correctly is only half the job. Nop, Put, PutCall and SwapCall all write at
// fn + 0xNN, and those offsets were read off Anniversary Edition codegen. VR 1.4.15 was compiled
// from a different revision, so the same instruction sits somewhere else, or in another function
// entirely. Writing to the AE offset then lands in the middle of an unrelated instruction, which
// corrupts code rather than failing.
//
// The client therefore declares the offset per build, in a `#if TP_SKYRIMVR` block of constexpr
// constants next to the patch. This reads both branches, prints what each build has at its own
// declared offset, and then tries to locate the SE instruction in the VR function independently.
// Where the search succeeds it either confirms the declared offset or contradicts it. Call patches
// are located by where the call goes rather than by bytes, which is exact: the SE target is mapped
// to VR through the ordinary chain and the VR function is scanned for a call to it.
//
// Sites are scraped from the client rather than listed here, so this cannot drift from the code.
// #if 0 is reported as disabled, and a patch that only one build applies is reported as such.
//
// Usage:  node Tools/vr_addresses/patches.mjs

import fs from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';
import { openImage, SE_PATH, VR_PATH, HAVE_UNPACKED } from './pe.mjs';
import { loadVersionLib } from './versionlib.mjs';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const REPO = path.resolve(HERE, '..', '..');
const CACHE = path.join(HERE, 'cache');
const SE_GAME = 'C:/Program Files (x86)/Steam/steamapps/common/Skyrim Special Edition';

if (!HAVE_UNPACKED) {
  console.error('needs unpacked binaries in cache/ (see the note in pe.mjs)');
  process.exit(2);
}

const hex = s => parseInt(s.trim().replace(/^0x/i, ''), 16);
const abs = rva => '0x' + (rva + 0x140000000).toString(16).toUpperCase();
const bytesOf = b => (b ? [...b].map(x => x.toString(16).padStart(2, '0')).join(' ') : '??');

// ---------------------------------------------------------------- scrape the client

const DECL = /VersionDbPtr\s*<[^>]*>\s+(\w+)\s*\(\s*(\d+)\s*\)/g;
const CONST = /constexpr\s+size_t\s+(\w+)\s*=\s*(0x[0-9a-fA-F]+|\d+)\s*;/;
// The call can be split over several lines, so this runs over the whole file rather than line by
// line, and the line number comes from the match position. The offset is a sum of terms, each
// either a literal or one of the per-build constants.
const PATCH = /TiltedPhoques::(Nop|PutCall|SwapCall|Put)\s*(?:<[^>]*>)?\s*\(\s*(?:mem::pointer\()?\s*(\w+)\s*\.\s*Get(?:Ptr|PtrU)?\s*\(\s*\)\s*\)?\s*((?:\+\s*\w+\s*)+),/g;

// Which build compiles each line, and whether it is inside an #if 0. Only the conditions that
// select a build are interpreted; anything else is treated as compiled by both.
function branches(lines) {
  const kindOf = line => {
    const m = /^\s*#\s*if\s+(.+?)\s*$/.exec(line);
    if (!m) return 'other';
    if (m[1] === '0') return 'dead';
    if (m[1] === 'TP_SKYRIMVR') return 'vr';
    if (m[1] === '!TP_SKYRIMVR' || m[1] === '! TP_SKYRIMVR' || m[1] === 'TP_SKYRIMSE') return 'se';
    return 'other';
  };

  const out = [];
  const stack = [];
  for (const line of lines) {
    if (/^\s*#\s*if/.test(line)) stack.push({ kind: kindOf(line), inElse: false });
    else if (/^\s*#\s*el(se|if)/.test(line) && stack.length) stack[stack.length - 1].inElse = true;
    else if (/^\s*#\s*endif/.test(line)) stack.pop();

    let dead = false, only = null;
    for (const f of stack) {
      if (f.kind === 'dead' && !f.inElse) dead = true;
      if (f.kind === 'vr') only = f.inElse ? 'se' : 'vr';
      if (f.kind === 'se') only = f.inElse ? 'vr' : 'se';
    }
    out.push({ dead, only });
  }
  return out;
}

function scrapeSites() {
  const sites = [];
  const walk = dir => {
    for (const ent of fs.readdirSync(dir, { withFileTypes: true })) {
      const p = path.join(dir, ent.name);
      if (ent.isDirectory()) { walk(p); continue; }
      if (!/\.cpp$/.test(ent.name)) continue;

      const rel = path.relative(REPO, p).replace(/\\/g, '/');
      const text = fs.readFileSync(p, 'utf8');
      const lines = text.split('\n');
      const where = branches(lines);

      const ids = new Map();
      for (const d of text.matchAll(DECL)) ids.set(d[1], +d[2]);

      // Offset constants, per build. One declared outside a build branch counts for both.
      const consts = { se: new Map(), vr: new Map() };
      lines.forEach((line, i) => {
        const m = CONST.exec(line);
        if (!m || where[i].dead) return;
        const value = m[2].startsWith('0x') ? hex(m[2]) : +m[2];
        for (const b of where[i].only ? [where[i].only] : ['se', 'vr']) consts[b].set(m[1], value);
      });

      const resolve = (expr, build) => {
        let sum = 0;
        for (const t of expr.match(/\w+/g)) {
          if (/^0x/i.test(t)) sum += hex(t);
          else if (/^\d+$/.test(t)) sum += +t;
          else if (consts[build].has(t)) sum += consts[build].get(t);
          else return null;
        }
        return sum;
      };

      // start offset of each line, for turning a match index into a line number
      const lineAt = [];
      for (let i = 0, at = 0; i < lines.length; i++) { lineAt.push(at); at += lines[i].length + 1; }

      for (const m of text.matchAll(PATCH)) {
        let ln = lineAt.findIndex(a => a > m.index) - 1;
        if (ln < 0) ln = lines.length - 1;
        if (/^\s*\/\//.test(lines[ln])) continue;
        sites.push({
          site: `${rel}:${ln + 1}`,
          op: m[1],
          name: m[2],
          id: ids.get(m[2]),
          seOff: resolve(m[3], 'se'),
          vrOff: resolve(m[3], 'vr'),
          disabled: where[ln].dead,
          only: where[ln].only,
        });
      }
    }
  };
  walk(path.join(REPO, 'Code', 'client'));
  return sites;
}

// ---------------------------------------------------------------- images

const lib = loadVersionLib(path.join(SE_GAME, 'Data/SKSE/Plugins/versionlib-1-6-1170-0.bin'));
const se = openImage(SE_PATH);
const vr = openImage(VR_PATH);

// next function boundary after an address, so a search stays inside the function
function boundaries(img) {
  const pd = img.sections.find(s => s.name === '.pdata');
  const n = Math.floor(Math.min(pd.virtualSize, pd.rawSize) / 12);
  const starts = [];
  for (let i = 0; i < n; i++) starts.push(img.buf.readUInt32LE(pd.rawOffset + i * 12));
  starts.sort((a, b) => a - b);
  return rva => starts.find(s => s > rva) ?? rva + 0x1000;
}
const seEnd = boundaries(se);
const vrEnd = boundaries(vr);

// SE 1.6.1170 rva -> VR rva, for resolving what a patched call points at
function callTargetMap() {
  const line = (file, cb) => {
    const t = fs.readFileSync(path.join(CACHE, file), 'latin1');
    let i = t.indexOf('\n') + 1;
    while (i < t.length) {
      let nl = t.indexOf('\n', i);
      if (nl < 0) nl = t.length;
      let e = nl;
      if (e > i && t.charCodeAt(e - 1) === 13) e--;
      if (e > i) cb(t.slice(i, e));
      i = nl + 1;
    }
  };
  const aeAddr = new Map(), sseOfAe = new Map(), vrOf = new Map();
  line('offsets-1-6-318-0.csv', l => { const c = l.indexOf(','); aeAddr.set(+l.slice(0, c), hex(l.slice(c + 1))); });
  line('se-ae-attempted-match.csv', l => { const c = l.indexOf(','); const a = hex(l.slice(c + 1)); if (!sseOfAe.has(a)) sseOfAe.set(a, hex(l.slice(0, c))); });
  line('sse_vr.csv', l => { const c = l.indexOf(','); vrOf.set(hex(l.slice(0, c)), hex(l.slice(c + 1))); });

  const map = new Map();
  for (const [id, seOff] of lib.offsets) {
    const v = vrOf.get(sseOfAe.get(aeAddr.get(id)));
    if (v !== undefined) map.set(seOff, v - 0x140000000);
  }
  return map;
}
const seToVr = callTargetMap();

// ---------------------------------------------------------------- the VR address table

const report = fs.readFileSync(path.join(HERE, 'report.tsv'), 'utf8').trim().split('\n');
const cols = report[0].split('\t');
const vrOfId = new Map();
for (const l of report.slice(1)) {
  const r = Object.fromEntries(l.split('\t').map((v, i) => [cols[i], v]));
  if (r.vr_offset) vrOfId.set(+r.aeid, { rva: parseInt(r.vr_offset, 16), conf: r.confidence, name: r.name });
}

// ---------------------------------------------------------------- checks

// A call patch is pinned by its destination, not its bytes.
function findCall(img, from, to, wantTarget) {
  const out = [];
  for (let rva = from; rva < to; rva++) {
    const b = img.read(rva, 5);
    if (!b || b.length < 5 || b[0] !== 0xe8) continue;
    if (rva + 5 + b.readInt32LE(1) === wantTarget) out.push(rva);
  }
  return out;
}

// Everything else is matched on the instruction bytes themselves, taken with a little context so
// a short opcode does not match everywhere. Positions listed in `skip` are wildcards.
function findBytes(img, from, to, needle, skip = []) {
  const out = [];
  if (!needle) return out;
  const buf = img.read(from, to - from + needle.length);
  if (!buf) return out;
  for (let i = 0; i + needle.length <= buf.length; i++) {
    let ok = true;
    for (let k = 0; k < needle.length; k++) {
      if (skip.includes(k)) continue;
      if (buf[i + k] !== needle[k]) { ok = false; break; }
    }
    if (ok) out.push(from + i);
  }
  return out;
}

// A branch keeps its opcode across the recompile but not its displacement, so the displacement
// bytes have to be wildcards or nothing matches. Only the forms the client actually patches are
// recognised; anything else is compared byte for byte.
function branchDisplacement(bytes) {
  if (!bytes || !bytes.length) return [];
  if (bytes[0] === 0x0f && bytes[1] >= 0x80 && bytes[1] <= 0x8f) return [2, 3, 4, 5];
  if ((bytes[0] >= 0x70 && bytes[0] <= 0x7f) || bytes[0] === 0xeb) return [1];
  if (bytes[0] === 0xe8 || bytes[0] === 0xe9) return [1, 2, 3, 4];
  return [];
}

const seImports = se.imports();
const vrImports = vr.imports();

// A call shortly before the patch is the sturdiest landmark there is, because the recompile
// reordered code and changed encodings but not who calls whom. Take that call, find the same one
// in the VR function, and the patch should sit the same number of bytes past it. Where the
// function makes that call more than once, the occurrence has to line up too.
//
// A direct call is identified by its target, mapped through the ordinary chain; an indirect call
// is identified by the name it imports, which needs no mapping at all and so still works where
// the chain has nothing.
//
// This outranks byte comparison. VR re-encoded `cmp byte [rax+8], 0` as `cmp [rax+8], sil` in
// MainInit, so a byte window taken from SE steps over the real site and matches the next
// structurally identical gate instead. A call landmark is immune to that.
function landmarks(from, seAt) {
  const out = [];
  for (let rva = seAt - 5; rva >= seAt - 32 && rva >= from; rva--) {
    const b = se.read(rva, 6);
    if (!b || b.length < 6) continue;
    if (b[0] === 0xe8) {
      const target = rva + 5 + b.readInt32LE(1);
      const vrTarget = seToVr.get(target);
      if (vrTarget !== undefined) out.push({ rva, len: 5, target, vrTarget, label: abs(target) });
    } else if (b[0] === 0xff && b[1] === 0x15) {
      const name = seImports.get(rva + 6 + b.readInt32LE(2));
      if (name) out.push({ rva, len: 6, name, label: name });
    }
  }
  return out;
}

function landmarkSites(img, from, to, mark, imports, target) {
  const out = [];
  for (let rva = from; rva < to; rva++) {
    const b = img.read(rva, 6);
    if (!b || b.length < 6) continue;
    if (mark.name === undefined) {
      if (b[0] === 0xe8 && rva + 5 + b.readInt32LE(1) === target) out.push(rva);
    } else if (b[0] === 0xff && b[1] === 0x15 && imports.get(rva + 6 + b.readInt32LE(2)) === mark.name) {
      out.push(rva);
    }
  }
  return out;
}

function locateByLandmark(seFn, seAt, vrFn, vrTo) {
  const seTo = seEnd(seFn);
  for (const mark of landmarks(seFn, seAt)) {
    const seSites = landmarkSites(se, seFn, seTo, mark, seImports, mark.target);
    const vrSites = landmarkSites(vr, vrFn, vrTo, mark, vrImports, mark.vrTarget);
    const which = seSites.indexOf(mark.rva);
    if (which < 0 || seSites.length !== vrSites.length) continue;
    return { at: vrSites[which] + mark.len + (seAt - mark.rva - mark.len), mark, which: which + 1, of: seSites.length };
  }
  return null;
}

// Finds the one place in the VR function that looks like the SE patch site, comparing a window of
// bytes on both sides of it. Context before the patch pins sites that the bytes after cannot: a
// function can hold two `test eax,eax / jz / mov rdx,[rip+X]` blocks and differ only in what
// leads into them. Every displacement in the window is a wildcard, since those all moved.
function locate(seAt, vrFrom, vrTo) {
  for (const before of [8, 4, 0]) {
    for (let after = 8; after >= 3; after--) {
      const window = se.read(seAt - before, before + after);
      if (!window) continue;

      const skip = branchDisplacement(se.read(seAt, 2)).map(k => k + before);
      for (let i = 0; i + 5 <= before; i++) if (window[i] === 0xe8) skip.push(i + 1, i + 2, i + 3, i + 4);

      const hits = findBytes(vr, vrFrom - before, vrTo, window, skip);
      if (hits.length === 1) return { at: hits[0] + before, before, after };
    }
  }
  return null;
}

const sites = scrapeSites().filter(s => s.seOff > 0 || s.vrOff > 0);
let contradicted = 0, confirmed = 0, unknown = 0, skipped = 0;

console.log(`${sites.length} in-function patch sites in the client\n`);

for (const s of sites) {
  const off = b => (s[b] === null ? '?' : '0x' + s[b].toString(16));
  console.log(`--- ${s.site}  ${s.op}  ${s.name}(${s.id})  SE +${off('seOff')} / VR +${off('vrOff')}`);
  if (s.disabled) { console.log('    inside #if 0, not applied\n'); skipped++; continue; }
  if (s.only === 'se') { console.log('    SkyrimSE only, VR does not apply this patch\n'); skipped++; continue; }

  const seFn = lib.offsets.get(s.id);
  const vrFn = vrOfId.get(s.id);
  if (seFn === undefined) { console.log('    id not in the SE versionlib\n'); unknown++; continue; }
  if (!vrFn) { console.log('    id has no VR mapping yet\n'); unknown++; continue; }
  if (s.seOff === null || s.vrOff === null) { console.log('    the offset does not resolve to a constant\n'); unknown++; continue; }

  const seAt = seFn + s.seOff;
  const vrAt = vrFn.rva + s.vrOff;
  console.log(`    SE ${abs(seFn)} + ${off('seOff')} = ${abs(seAt)}   ${bytesOf(se.read(seAt, 8))}`);
  console.log(`    VR ${abs(vrFn.rva)} + ${off('vrOff')} = ${abs(vrAt)}   ${bytesOf(vr.read(vrAt, 8))}   [${vrFn.conf}]`);

  const vrFrom = vrFn.rva, vrTo = vrEnd(vrFn.rva);
  let hits = [], how;

  // A call patch is pinned by its destination, which is exact wherever the destination maps.
  if (s.op === 'SwapCall' || s.op === 'PutCall') {
    const b = se.read(seAt, 5);
    if (!b || b[0] !== 0xe8) {
      console.log(`    SE has no direct call here, cannot check\n`);
      unknown++;
      continue;
    }
    const seTarget = seAt + 5 + b.readInt32LE(1);
    const vrTarget = seToVr.get(seTarget);
    const at = vr.read(vrAt, 5);
    const declaredTarget = at && at[0] === 0xe8 ? abs(vrAt + 5 + at.readInt32LE(1)) : 'nothing, it is not a direct call';
    console.log(`    call goes to SE ${abs(seTarget)} -> VR ${vrTarget === undefined ? 'unmapped' : abs(vrTarget)}, and the declared offset calls ${declaredTarget}`);
    if (vrTarget !== undefined) {
      hits = findCall(vr, vrFrom, vrTo, vrTarget);
      how = 'call to the same function';
    }
    // otherwise fall through: the site can still be placed without knowing where the call goes
  }

  if (!how) {
    const byMark = locateByLandmark(seFn, seAt, vrFrom, vrTo);
    const found = byMark ?? locate(seAt, vrFrom, vrTo);
    if (byMark) {
      hits = [byMark.at];
      how = `the call to ${byMark.mark.label} before it (occurrence ${byMark.which} of ${byMark.of})`;
    } else if (found) {
      hits = [found.at];
      how = `the surrounding bytes, ${found.before} before and ${found.after} after`;
    } else {
      // Nothing unique. Fall back to a forward-only search so the report can at least name the
      // plausible sites, keeping the longest needle that matched anything.
      const skip = branchDisplacement(se.read(seAt, 2));
      for (let len = 8; len >= 3; len--) {
        const f = findBytes(vr, vrFrom, vrTo, se.read(seAt, len), skip);
        if (f.length && !hits.length) hits = f;
      }
      how = 'the patched instruction alone';
    }
  }

  if (!hits.length) {
    console.log(`    the search found nothing like it in ${abs(vrFrom)}..${abs(vrTo)} by ${how}`);
    console.log('    the search cannot settle this one, it rests on a hand comparison\n');
    unknown++;
  } else if (hits.length > 1) {
    const declared = hits.some(h => h - vrFrom === s.vrOff);
    console.log(`    ambiguous, ${hits.length} matches by ${how}: ${hits.map(h => '+0x' + (h - vrFrom).toString(16)).join(' ')}`);
    console.log(`    ${declared ? 'the declared offset is among them, but the search does not single it out' : 'NONE of them is the declared offset'}\n`);
    if (declared) unknown++;
    else contradicted++;
  } else {
    const found = hits[0] - vrFrom;
    if (found === s.vrOff) {
      console.log(`    located by ${how} at VR +0x${found.toString(16)}, which is what the client declares\n`);
      confirmed++;
    } else {
      console.log(`    located by ${how} at VR +0x${found.toString(16)}, but the client declares +${off('vrOff')}`);
      console.log('    CONTRADICTED, one of the two is wrong\n');
      contradicted++;
    }
  }
}

console.log(`\n${confirmed} confirmed by search, ${contradicted} contradicted, ${unknown} declared but unconfirmed, ${skipped} not applied`);
