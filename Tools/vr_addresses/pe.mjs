// Minimal PE64 reader, enough to turn an RVA into bytes for SkyrimSE.exe and SkyrimVR.exe.
import fs from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';

const CACHE = path.join(path.dirname(fileURLToPath(import.meta.url)), 'cache');

// Both retail exes are wrapped in SteamStub, so .text is encrypted on disk. Anything that needs
// instruction bytes requires an unpacked copy in cache/, produced with Steamless:
//   Steamless.CLI.exe --quiet <copy of SkyrimVR.exe>
// then rename the .unpacked.exe output as below. The packed originals still serve for .rdata,
// .data and .pdata, so the tools degrade rather than fail when the copies are absent.
const RETAIL_SE = 'C:/Program Files (x86)/Steam/steamapps/common/Skyrim Special Edition/SkyrimSE.exe';
const RETAIL_VR = 'C:/Program Files (x86)/Steam/steamapps/common/SkyrimVR/SkyrimVR.exe';

const pick = (unpacked, retail) => {
  const u = path.join(CACHE, unpacked);
  return fs.existsSync(u) ? u : retail;
};

export const SE_PATH = pick('SkyrimSE.unpacked.exe', RETAIL_SE);
export const VR_PATH = pick('SkyrimVR.unpacked.exe', RETAIL_VR);
export const HAVE_UNPACKED = SE_PATH !== RETAIL_SE && VR_PATH !== RETAIL_VR;

export function openImage(file) {
  const buf = fs.readFileSync(file);
  if (buf.readUInt16LE(0) !== 0x5a4d) throw new Error(`${file}: not a DOS image`);

  const pe = buf.readUInt32LE(0x3c);
  if (buf.readUInt32LE(pe) !== 0x00004550) throw new Error(`${file}: bad PE signature`);

  const numSections = buf.readUInt16LE(pe + 6);
  const optSize = buf.readUInt16LE(pe + 20);
  const opt = pe + 24;
  if (buf.readUInt16LE(opt) !== 0x20b) throw new Error(`${file}: not PE32+`);

  const imageBase = Number(buf.readBigUInt64LE(opt + 24));
  const sections = [];
  for (let i = 0; i < numSections; i++) {
    const s = opt + optSize + i * 40;
    sections.push({
      name: buf.toString('latin1', s, s + 8).replace(/\0+$/, ''),
      virtualSize: buf.readUInt32LE(s + 8),
      virtualAddress: buf.readUInt32LE(s + 12),
      rawSize: buf.readUInt32LE(s + 16),
      rawOffset: buf.readUInt32LE(s + 20),
      characteristics: buf.readUInt32LE(s + 36),
    });
  }

  const sectionOf = rva => sections.find(s => rva >= s.virtualAddress && rva < s.virtualAddress + Math.max(s.virtualSize, s.rawSize));

  const toOffset = rva => {
    const s = sectionOf(rva);
    if (!s) return -1;
    const off = s.rawOffset + (rva - s.virtualAddress);
    return off < buf.length ? off : -1;
  };

  const dataDir = i => {
    const n = buf.readUInt32LE(opt + 108);
    return i < n ? [buf.readUInt32LE(opt + 112 + i * 8), buf.readUInt32LE(opt + 116 + i * 8)] : [0, 0];
  };

  return {
    path: file,
    buf,
    imageBase,
    sections,
    sectionOf,
    toOffset,
    entryPoint: buf.readUInt32LE(opt + 16),
    // rva of every IAT slot -> "dll!symbol"
    imports() {
      const cstr = rva => {
        const o = toOffset(rva);
        let z = o;
        while (buf[z]) z++;
        return buf.toString('latin1', o, z);
      };
      const out = new Map();
      for (let d = toOffset(dataDir(1)[0]); ; d += 20) {
        const lookupRva = buf.readUInt32LE(d) || buf.readUInt32LE(d + 16);
        const nameRva = buf.readUInt32LE(d + 12);
        const iatRva = buf.readUInt32LE(d + 16);
        if (!nameRva && !iatRva) break;
        const dll = cstr(nameRva);
        for (let t = toOffset(lookupRva), slot = iatRva; ; t += 8, slot += 8) {
          const v = buf.readBigUInt64LE(t);
          if (v === 0n) break;
          out.set(slot, `${dll}!${v & (1n << 63n) ? `#${Number(v & 0xffffn)}` : cstr(Number(v & 0x7fffffffn) + 2)}`);
        }
      }
      return out;
    },
    // rva may be given as an absolute address; anything above the image base is normalised
    norm: a => (a >= imageBase ? a - imageBase : a),
    read(rva, len) {
      const off = this.toOffset(this.norm(rva));
      if (off < 0) return null;
      return buf.subarray(off, Math.min(off + len, buf.length));
    },
    isExecutable(rva) {
      const s = sectionOf(this.norm(rva));
      return !!s && (s.characteristics & 0x20000000) !== 0; // IMAGE_SCN_MEM_EXECUTE
    },
  };
}

export function hexdump(bytes, rva) {
  const out = [];
  for (let i = 0; i < bytes.length; i += 16) {
    const row = bytes.subarray(i, i + 16);
    const hex = [...row].map(b => b.toString(16).padStart(2, '0')).join(' ').padEnd(47);
    const asc = [...row].map(b => (b >= 0x20 && b < 0x7f ? String.fromCharCode(b) : '.')).join('');
    out.push(`  ${(rva + i).toString(16)}  ${hex}  ${asc}`);
  }
  return out.join('\n');
}

// Decodes just enough to recognise a singleton getter: the RIP-relative lea or mov that loads
// the address of a global, and the direct call/jmp targets used for cross-checking a mapping.
// Returns [{ rva, len, text, target? }].
const REGS64 = ['rax', 'rcx', 'rdx', 'rbx', 'rsp', 'rbp', 'rsi', 'rdi', 'r8', 'r9', 'r10', 'r11', 'r12', 'r13', 'r14', 'r15'];

export function decode(bytes, rva, maxInstructions = 12) {
  const out = [];
  let i = 0;

  while (i < bytes.length && out.length < maxInstructions) {
    const start = i;
    let rex = 0;
    while (i < bytes.length && bytes[i] >= 0x40 && bytes[i] <= 0x4f) rex = bytes[i++];
    const op = bytes[i];
    const rexR = (rex & 4) !== 0;
    let text = null, target;

    if ((op === 0x8d || op === 0x8b) && i + 6 <= bytes.length) {
      // lea / mov r64, m -- only the [rip+disp32] form is of interest here
      const modrm = bytes[i + 1];
      if ((modrm >> 6) === 0 && (modrm & 7) === 5) {
        const disp = bytes.readInt32LE(i + 2);
        i += 6;
        target = rva + i + disp; // RIP points at the next instruction
        text = `${op === 0x8d ? 'lea' : 'mov'} ${REGS64[((modrm >> 3) & 7) + (rexR ? 8 : 0)]}, [rip+0x${disp.toString(16)}]  -> 0x${target.toString(16)}`;
      }
    } else if ((op === 0xe8 || op === 0xe9) && i + 5 <= bytes.length) {
      const disp = bytes.readInt32LE(i + 1);
      i += 5;
      target = rva + i + disp;
      text = `${op === 0xe8 ? 'call' : 'jmp'} 0x${target.toString(16)}`;
    } else if (op === 0xc3) {
      i += 1;
      text = 'ret';
    } else if (op === 0xcc) {
      i += 1;
      text = 'int3';
    }

    if (text === null) break; // stop at the first thing this tiny decoder cannot read
    out.push({ rva: rva + start, len: i - start, text, target });
  }
  return out;
}
