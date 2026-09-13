// Locates a class vtable through its MSVC RTTI records and dumps its slots, for both builds.
//
// This is derivation from the binary rather than a mapping, so it is authoritative, and it answers
// a question the id chain cannot: which function is slot N of this class. Where the client hooks a
// virtual, dumping the same class in both builds pairs the two addresses directly, and the slot
// index has to agree for the pairing to be believed.
//
// The chain of records is:
//   .data  ".?AVName@@" is inside a TypeDescriptor, which starts 16 bytes earlier
//   .rdata a RTTICompleteObjectLocator holds that descriptor's rva at +0xC and its own at +0x14
//   .rdata the 8 byte pointer to the locator sits immediately before the vtable
// A class with several bases has one locator per base, each with its own vtable; the locator's
// `offset` field says where that base sits inside the object, which is also the displacement the
// base's own methods use to recover `this`.
//
// Usage:  node Tools/vr_addresses/vtable.mjs <se|vr> <ClassName> [slots]
//         node Tools/vr_addresses/vtable.mjs both StatsMenu 12

import { openImage, SE_PATH, VR_PATH } from './pe.mjs';

const [which, cls, slotArg] = process.argv.slice(2);
if (!which || !cls) {
  console.error('usage: vtable.mjs <se|vr|both> <ClassName> [slots]');
  process.exit(2);
}
const slots = Number(slotArg || 8);
const abs = rva => '0x' + (rva + 0x140000000).toString(16).toUpperCase();

function dump(path, label) {
  const img = openImage(path);
  const sec = name => img.sections.find(s => s.name === name);
  const rvaOf = (s, off) => off - s.rawOffset + s.virtualAddress;
  const extent = s => [s.rawOffset, s.rawOffset + Math.min(s.rawSize, s.virtualSize)];

  console.log(`\n=== ${label}  ${cls}`);

  // 1. type descriptors carrying the mangled name
  const needle = Buffer.from(`.?AV${cls}@@\0`, 'latin1');
  const data = sec('.data');
  const [dFrom, dTo] = extent(data);
  const descriptors = [];
  for (let i = dFrom; ; ) {
    i = img.buf.indexOf(needle, i);
    if (i < 0 || i >= dTo) break;
    descriptors.push(rvaOf(data, i) - 16);
    i++;
  }
  if (!descriptors.length) {
    console.log(`  no type descriptor, ${cls} does not exist in this build`);
    return;
  }

  const rdata = sec('.rdata');
  const [rFrom, rTo] = extent(rdata);

  for (const td of descriptors) {
    console.log(`  type descriptor ${abs(td)}`);

    // 2. locators pointing at it, confirmed by the self pointer at +0x14
    for (let o = rFrom; o + 4 <= rTo; o += 4) {
      if (img.buf.readUInt32LE(o) !== td || o - 0xc < rFrom) continue;
      const locator = rvaOf(rdata, o - 0xc);
      if (img.buf.readUInt32LE(o + 8) !== locator) continue;

      const baseOffset = img.buf.readUInt32LE(o - 8);
      console.log(`    locator ${abs(locator)}  base offset 0x${baseOffset.toString(16)}`);

      // 3. the pointer to the locator, with the vtable right behind it
      const want = BigInt(locator + 0x140000000);
      for (let p = rFrom; p + 8 <= rTo; p += 8) {
        if (img.buf.readBigUInt64LE(p) !== want) continue;
        console.log(`      vtable ${abs(rvaOf(rdata, p + 8))}`);
        for (let i = 0; i < slots && p + 16 + i * 8 <= rTo; i++)
          console.log(`        [${String(i).padStart(2)}] 0x${img.buf.readBigUInt64LE(p + 8 + i * 8).toString(16).toUpperCase()}`);
      }
    }
  }
}

if (which === 'se' || which === 'both') dump(SE_PATH, 'SkyrimSE 1.6.1170');
if (which === 'vr' || which === 'both') dump(VR_PATH, 'SkyrimVR 1.4.15');
