// Reads meh321's versionlib-*.bin, so an AE id can be turned into an address for the exact
// SkyrimSE build installed here rather than the 1.6.318 the CSVs are dumped from.
// Format 2, same decoding as VersionDb::Load in Code/client/VersionDb.h.
import fs from 'fs';

export function loadVersionLib(file) {
  const buf = fs.readFileSync(file);
  let p = 0;
  const i32 = () => { const v = buf.readInt32LE(p); p += 4; return v; };
  const u8 = () => buf[p++];
  const u16 = () => { const v = buf.readUInt16LE(p); p += 2; return v; };
  const u32 = () => { const v = buf.readUInt32LE(p); p += 4; return v; };
  const u64 = () => { const v = Number(buf.readBigUInt64LE(p)); p += 8; return v; };

  const format = i32();
  if (format !== 2) throw new Error(`${file}: unsupported format ${format}`);

  const ver = [i32(), i32(), i32(), i32()];
  const nameLen = i32();
  const moduleName = nameLen > 0 ? buf.toString('latin1', p, p + nameLen) : '';
  p += Math.max(0, nameLen);

  const ptrSize = i32();
  const count = i32();

  const offsets = new Map();
  let pvid = 0, poffset = 0;

  for (let i = 0; i < count; i++) {
    const type = u8();
    const low = type & 0xf, high = type >> 4;

    let id;
    switch (low) {
      case 0: id = u64(); break;
      case 1: id = pvid + 1; break;
      case 2: id = pvid + u8(); break;
      case 3: id = pvid - u8(); break;
      case 4: id = pvid + u16(); break;
      case 5: id = pvid - u16(); break;
      case 6: id = u16(); break;
      case 7: id = u32(); break;
      default: throw new Error(`bad low nibble ${low} at entry ${i}`);
    }

    const tp = (high & 8) !== 0 ? poffset / ptrSize : poffset;
    let off;
    switch (high & 7) {
      case 0: off = u64(); break;
      case 1: off = tp + 1; break;
      case 2: off = tp + u8(); break;
      case 3: off = tp - u8(); break;
      case 4: off = tp + u16(); break;
      case 5: off = tp - u16(); break;
      case 6: off = u16(); break;
      case 7: off = u32(); break;
    }
    if ((high & 8) !== 0) off *= ptrSize;

    offsets.set(id, off);
    pvid = id;
    poffset = off;
  }

  return { version: ver.join('.'), moduleName, ptrSize, offsets };
}
