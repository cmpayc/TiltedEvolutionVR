// Reads process memory out of a coredump, so an object a crash was holding can be identified.
//
// stack.mjs answers "which call was it". This answers "what was it holding": the mod writes a full
// memory dump, so a pointer in the register set can be followed. A game object's vtable names its
// class through the RTTI records, and a TESForm carries its form id at +0x14, which is usually
// enough to say which actor or reference the game was working on.
//
// Usage:
//   node Tools/vr_addresses/dumpmem.mjs <dump> <address> [length]   hex dump, decoded if executable
//   node Tools/vr_addresses/dumpmem.mjs <dump> <address> --form     read it as a TESForm
//   node Tools/vr_addresses/dumpmem.mjs <dump> <address> --class    name it from its vtable

import fs from 'fs';
import { openImage, hexdump, VR_PATH } from './pe.mjs';

const [dumpPath, addrArg, arg3] = process.argv.slice(2);
if (!dumpPath || !addrArg) {
  console.error('usage: dumpmem.mjs <dump> <address> [length|--form|--class]');
  process.exit(2);
}

const fd = fs.openSync(dumpPath, 'r');
const rd = (off, len) => { const b = Buffer.alloc(len); fs.readSync(fd, b, 0, len, off); return b; };

const head = rd(0, 32);
if (head.toString('latin1', 0, 4) !== 'MDMP') throw new Error(`${dumpPath}: not a minidump`);
const nStreams = head.readUInt32LE(8);
const dir = rd(head.readUInt32LE(12), nStreams * 12);
const streams = new Map();
for (let i = 0; i < nStreams; i++)
  streams.set(dir.readUInt32LE(i * 12), { size: dir.readUInt32LE(i * 12 + 4), rva: dir.readUInt32LE(i * 12 + 8) });

// Two shapes carry memory. Memory64ListStream is a count, the file offset its data starts at, then
// (start, size) pairs laid out contiguously from there. MemoryListStream, which is what the mod's
// dumps actually use, carries an explicit file offset per range instead.
const ranges = [];
const m64 = streams.get(9);
const mem = streams.get(5);
if (m64) {
  const nRanges = Number(rd(m64.rva, 8).readBigUInt64LE(0));
  let cursor = Number(rd(m64.rva + 8, 8).readBigUInt64LE(0));
  const descs = rd(m64.rva + 16, nRanges * 16);
  for (let i = 0; i < nRanges; i++) {
    const start = Number(descs.readBigUInt64LE(i * 16));
    const size = Number(descs.readBigUInt64LE(i * 16 + 8));
    ranges.push({ start, size, off: cursor });
    cursor += size;
  }
} else if (mem) {
  const nRanges = rd(mem.rva, 4).readUInt32LE(0);
  const descs = rd(mem.rva + 4, nRanges * 16);
  for (let i = 0; i < nRanges; i++)
    ranges.push({
      start: Number(descs.readBigUInt64LE(i * 16)),
      size: descs.readUInt32LE(i * 16 + 8),
      off: descs.readUInt32LE(i * 16 + 12),
    });
} else {
  throw new Error('this dump carries no memory stream');
}
ranges.sort((a, b) => a.start - b.start);
const nRanges = ranges.length;

function read(addr, len) {
  let lo = 0, hi = ranges.length - 1;
  while (lo <= hi) {
    const mid = (lo + hi) >> 1;
    const r = ranges[mid];
    if (addr < r.start) hi = mid - 1;
    else if (addr >= r.start + r.size) lo = mid + 1;
    else return rd(r.off + (addr - r.start), Math.min(len, r.start + r.size - addr));
  }
  return null;
}
const qword = a => { const b = read(a, 8); return b && b.length === 8 ? Number(b.readBigUInt64LE(0)) : null; };
const dword = a => { const b = read(a, 4); return b && b.length === 4 ? b.readUInt32LE(0) : null; };
const hx = v => (v === null ? 'unreadable' : '0x' + v.toString(16));

// vtable -> mangled class name, through the complete object locator that sits just before it
const vr = openImage(VR_PATH);
function className(vtable) {
  const loc = vr.read(vtable - 8, 8);
  if (!loc) return null;
  const locRva = vr.norm(Number(loc.readBigUInt64LE(0)));
  const o = vr.toOffset(locRva);
  if (o < 0) return null;
  const td = vr.toOffset(vr.buf.readUInt32LE(o + 12));
  if (td < 0) return null;
  let z = td + 16;
  while (vr.buf[z]) z++;
  return vr.buf.toString('latin1', td + 16, z);
}

const addr = Number(addrArg);
console.log(`${dumpPath}\n${nRanges} memory ranges, ${(ranges.reduce((s, r) => s + r.size, 0) / (1 << 20)).toFixed(0)} MB\n`);

if (arg3 === '--class' || arg3 === '--form') {
  const vt = qword(addr);
  console.log(`  vtable      ${hx(vt)}   ${vt ? className(vt) || '<no RTTI>' : ''}`);
  if (arg3 === '--form') {
    console.log(`  flags  +10  ${hx(dword(addr + 0x10))}`);
    console.log(`  formID +14  ${hx(dword(addr + 0x14))}`);
    console.log(`  formType +1A ${hx(read(addr + 0x1a, 1)?.[0])}`);
  }
} else {
  const len = Number(arg3 || 64);
  const b = read(addr, len);
  if (!b) { console.log('  unreadable'); process.exit(1); }
  console.log(hexdump(b, addr));
}
