// Reads bytes out of SkyrimSE.exe or SkyrimVR.exe at a given address, for confirming an
// address by hand before it goes into overrides.csv.
//
//   node Tools/vr_addresses/inspect.mjs vr 0x14010d590 [length]
//   node Tools/vr_addresses/inspect.mjs se 0x1405d29f0 64
//   node Tools/vr_addresses/inspect.mjs sections vr

import { openImage, hexdump, decode, SE_PATH, VR_PATH } from './pe.mjs';

const [which, addrArg, lenArg] = process.argv.slice(2);
if (!which) {
  console.error('usage: inspect.mjs <se|vr|sections> <address> [length]');
  process.exit(2);
}

if (which === 'sections') {
  const img = openImage(addrArg === 'se' ? SE_PATH : VR_PATH);
  console.log(`${img.path}\nimage base 0x${img.imageBase.toString(16)}`);
  for (const s of img.sections) {
    console.log(`  ${s.name.padEnd(8)} rva 0x${s.virtualAddress.toString(16).padStart(8, '0')}  vsize 0x${s.virtualSize.toString(16).padStart(8, '0')}  raw 0x${s.rawOffset.toString(16).padStart(8, '0')}  exec=${(s.characteristics & 0x20000000) !== 0}`);
  }
  process.exit(0);
}

const img = openImage(which === 'se' ? SE_PATH : VR_PATH);
const addr = Number(addrArg);
const len = lenArg ? Number(lenArg) : 48;
const rva = img.norm(addr);
const bytes = img.read(rva, len);

if (!bytes) {
  console.error(`0x${addr.toString(16)} (rva 0x${rva.toString(16)}) is not inside any section of ${img.path}`);
  process.exit(1);
}

const section = img.sectionOf(rva);
console.log(`${img.path}`);
console.log(`0x${addr.toString(16)}  rva 0x${rva.toString(16)}  section ${section.name}  exec=${img.isExecutable(rva)}`);

// Padding or a ret right before an address is good evidence that it is a function start.
const before = img.read(rva - 16, 16);
if (before) {
  const last = before[15];
  const pad = last === 0xcc || last === 0x90 || last === 0xc3 || last === 0x00;
  console.log(`preceding byte 0x${last.toString(16).padStart(2, '0')} -> ${pad ? 'function boundary' : 'mid-function, suspicious'}`);
}

console.log('\nbytes:');
console.log(hexdump(bytes, rva + img.imageBase));

const ins = decode(bytes, rva + img.imageBase);
if (ins.length) {
  console.log('\ndecoded:');
  for (const x of ins) console.log(`  ${x.rva.toString(16)}  ${x.text}`);
}
