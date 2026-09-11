// Unwinds the crashing thread of a coredump into a real call stack, and names what it can.
//
// Visual Studio only shows source for our own frames, so a crash inside the game reads as
// "ThisCall.hpp line 11" with no indication of which call it was. This walks the x64 unwind info
// in .pdata instead, which needs no symbols, and annotates each frame:
//   game frames  -> the Address Library id and the client source line, from report.tsv
//   client frames -> the game functions that frame calls, which identifies it just as well
//
// The launcher reserves a 1 GB `.game` section so SkyrimVR.exe maps at its native 0x140000000,
// and the client's own code lands above it, so one address space holds both images and a frame is
// attributed by which range its rip falls in.
//
//   node Tools/vr_addresses/stack.mjs <coredump.dmp> [path to SkyrimTogether.exe]

import fs from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';
import { openImage, VR_PATH } from './pe.mjs';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const REPO = path.resolve(HERE, '..', '..');

const dumpPath = process.argv[2];
const clientPath = process.argv[3] || path.join(REPO, 'build/windows/x64/releasedbg/SkyrimTogether.exe');
if (!dumpPath) {
  console.error('usage: stack.mjs <coredump.dmp> [SkyrimTogether.exe]');
  process.exit(2);
}

const hx = v => '0x' + v.toString(16);

// ---------------------------------------------------------------- coredump

const fd = fs.openSync(dumpPath, 'r');
const rd = (off, len) => { const b = Buffer.alloc(len); fs.readSync(fd, b, 0, len, off); return b; };

const head = rd(0, 32);
if (head.toString('latin1', 0, 4) !== 'MDMP') throw new Error(`${dumpPath}: not a minidump`);
const dir = rd(head.readUInt32LE(12), head.readUInt32LE(8) * 12);
const streams = new Map();
for (let i = 0; i < head.readUInt32LE(8); i++) {
  streams.set(dir.readUInt32LE(i * 12), { size: dir.readUInt32LE(i * 12 + 4), rva: dir.readUInt32LE(i * 12 + 8) });
}

const exStream = rd(streams.get(6).rva, 168);
const crashTid = exStream.readUInt32LE(0);
const exCode = exStream.readUInt32LE(8);
const exAddr = Number(exStream.readBigUInt64LE(24));
const faultAddr = Number(exStream.readBigUInt64LE(8 + 32 + 8));
const ctx = rd(exStream.readUInt32LE(164), 0x100);
const reg = o => Number(ctx.readBigUInt64LE(o));
const rip0 = reg(0xf8), rsp0 = reg(0x98);

// the crashing thread's captured stack
const tl = streams.get(3);
const nThreads = rd(tl.rva, 4).readUInt32LE(0);
const tb = rd(tl.rva + 4, nThreads * 48);
let stack = null;
for (let i = 0; i < nThreads; i++) {
  const o = i * 48;
  if (tb.readUInt32LE(o) === crashTid)
    stack = { start: Number(tb.readBigUInt64LE(o + 24)), size: tb.readUInt32LE(o + 32), rva: tb.readUInt32LE(o + 36) };
}
if (!stack) throw new Error('the crashing thread has no stack in this dump');
const sb = rd(stack.rva, stack.size);
const qword = a => (a >= stack.start && a + 8 <= stack.start + stack.size ? Number(sb.readBigUInt64LE(a - stack.start)) : null);

// ---------------------------------------------------------------- images

const BASE = 0x140000000;
const vr = openImage(VR_PATH);
const client = openImage(clientPath);
// everything below the end of the reserved .game section is the game image
const gameSection = client.sections.find(s => s.name === '.game');
const gameEnd = BASE + gameSection.virtualAddress + gameSection.virtualSize;
const imageOf = a => (a >= BASE && a < gameEnd ? { img: vr, tag: 'SkyrimVR.exe' } : a >= BASE ? { img: client, tag: 'client' } : null);

// .pdata as a sorted array, with a lookup for the entry covering an rva
function runtimeFunctions(img) {
  const p = img.sections.find(s => s.name === '.pdata');
  const n = Math.floor(Math.min(p.virtualSize, p.rawSize) / 12);
  const out = [];
  for (let i = 0; i < n; i++) {
    const o = p.rawOffset + i * 12;
    out.push([img.buf.readUInt32LE(o), img.buf.readUInt32LE(o + 4), img.buf.readUInt32LE(o + 8)]);
  }
  out.sort((a, b) => a[0] - b[0]);
  return out;
}
const rf = new Map([[vr, runtimeFunctions(vr)], [client, runtimeFunctions(client)]]);

function lookup(img, rva) {
  const a = rf.get(img);
  let l = 0, h = a.length - 1, f = -1;
  while (l <= h) { const m = (l + h) >> 1; if (a[m][0] <= rva) { f = m; l = m + 1; } else h = m - 1; }
  return f >= 0 && rva < a[f][1] ? a[f] : null;
}

// Total stack the function has taken below the return address, from its unwind codes. Good enough
// for a frame that is past its prologue, which every frame but the innermost always is.
function frameSize(img, entry) {
  let fn = entry, total = 0, guard = 0;
  while (fn && guard++ < 8) {
    const o = img.toOffset(fn[2]);
    const flags = img.buf[o] >> 3;
    const count = img.buf[o + 2];
    let i = 0;
    while (i < count) {
      const codeOff = o + 4 + i * 2;
      const op = img.buf[codeOff + 1] & 0xf, info = img.buf[codeOff + 1] >> 4;
      if (op === 0) { total += 8; i += 1; }
      else if (op === 1) { total += info === 0 ? img.buf.readUInt16LE(codeOff + 2) * 8 : img.buf.readUInt32LE(codeOff + 2); i += info === 0 ? 2 : 3; }
      else if (op === 2) { total += info * 8 + 8; i += 1; }
      else if (op === 3) i += 1;
      else if (op === 4 || op === 8) i += 2;
      else if (op === 5 || op === 9) i += 3;
      else if (op === 10) { total += info & 1 ? 0x30 : 0x28; i += 1; }
      else i += 1;
    }
    if (!(flags & 4)) break;
    const chain = o + 4 + ((count + 1) & ~1) * 2;
    fn = [img.buf.readUInt32LE(chain), img.buf.readUInt32LE(chain + 4), img.buf.readUInt32LE(chain + 8)];
  }
  return total;
}

// ---------------------------------------------------------------- naming

const byAddr = new Map();
for (const l of fs.readFileSync(path.join(HERE, 'report.tsv'), 'utf8').split('\n').slice(1)) {
  const f = l.split('\t');
  if (f[4]) byAddr.set(parseInt(f[4], 16), `id ${f[0]} ${f[2]} [${f[3]}]  ${f[10]}`);
}

// A client frame is identified by the game functions it calls: those have ids, and the ids carry
// the client source line that declared them.
function clientFrameHints(entry) {
  const hints = [];
  for (let rva = entry[0]; rva < entry[1]; rva++) {
    const o = client.toOffset(rva);
    if (client.buf[o] !== 0xe8) continue;
    const t = rva + 5 + client.buf.readInt32LE(o + 1) + BASE;
    const hit = byAddr.get(t);
    if (hit && !hints.includes(hit)) hints.push(hit);
  }
  // a call through a resolved pointer is an indirect call, so also try rip-relative loads of one
  for (let rva = entry[0]; rva < entry[1]; rva++) {
    const o = client.toOffset(rva);
    if (!(client.buf[o] >= 0x48 && client.buf[o] <= 0x4f && client.buf[o + 1] === 0x8b && (client.buf[o + 2] & 0xc7) === 0x05)) continue;
    const slot = rva + 7 + client.buf.readInt32LE(o + 3);
    const so = client.toOffset(slot);
    if (so < 0) continue;
    const hit = byAddr.get(Number(client.buf.readBigUInt64LE(so)));
    if (hit && !hints.includes(hit)) hints.push(hit);
  }
  return hints;
}

// ---------------------------------------------------------------- report

console.log(`${dumpPath}`);
console.log(`exception ${exCode.toString(16)} at ${hx(exAddr)}, faulting address ${hx(faultAddr)}, thread ${crashTid}`);
console.log('\nregisters');
for (const [name, off] of [['rax', 0x78], ['rcx', 0x80], ['rdx', 0x88], ['rbx', 0x90], ['rsp', 0x98], ['rbp', 0xa0], ['rsi', 0xa8], ['rdi', 0xb0], ['r8', 0xb8], ['r9', 0xc0], ['r12', 0xd8], ['r13', 0xe0], ['r14', 0xe8], ['r15', 0xf0]])
  console.log(`  ${name.padEnd(4)} ${hx(reg(off))}`);

console.log('\ncall stack, innermost first');
let rip = rip0, rsp = rsp0;
for (let depth = 0; depth < 40; depth++) {
  const where = imageOf(rip);
  if (!where) {
    // rip outside both images means a corrupted call target, usually a bad virtual dispatch. The
    // call pushed a return address and nothing else ran, so the caller is at the top of the stack.
    // This is the case where the caller is the whole point, so keep going rather than stopping.
    const ret = qword(rsp);
    console.log(`  ${String(depth).padStart(2)}  ${hx(rip)}  (not in either image: bad call target)`);
    if (ret === null || ret === 0) { console.log('          (stack ends here)'); break; }
    rip = ret;
    rsp += 8;
    continue;
  }
  const rva = rip - BASE;
  const entry = lookup(where.img, rva);
  if (!entry) {
    // No .pdata entry means a leaf: it never adjusts rsp, so the return address is simply on top.
    // Worth continuing rather than stopping, because a crash in a leaf is exactly when the caller
    // is the interesting frame.
    const ret = qword(rsp);
    console.log(`  ${String(depth).padStart(2)}  ${where.tag === 'SkyrimVR.exe' ? 'game  ' : 'client'}  ${hx(rip)}  (leaf, no unwind info)`);
    if (ret === null || ret === 0) { console.log('          (stack ends here)'); break; }
    rip = ret;
    rsp += 8;
    continue;
  }

  const fnAddr = entry[0] + BASE;
  const label = `${hx(fnAddr)}+0x${(rva - entry[0]).toString(16)}`;
  if (where.tag === 'SkyrimVR.exe') {
    const hit = byAddr.get(fnAddr);
    console.log(`  ${String(depth).padStart(2)}  game    ${label}${hit ? '   <<< ' + hit : ''}`);
  } else {
    const hints = clientFrameHints(entry);
    console.log(`  ${String(depth).padStart(2)}  client  ${label}`);
    for (const h of hints.slice(0, 4)) console.log(`          calls ${h}`);
  }

  const total = frameSize(where.img, entry);
  const retSlot = rsp + total;
  const ret = qword(retSlot);
  if (ret === null || ret === 0) { console.log('          (stack ends here)'); break; }
  rip = ret;
  rsp = retSlot + 8;
}
