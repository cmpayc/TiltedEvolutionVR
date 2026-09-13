// Every rip-relative reference to one address, classified as read, write or address-of.
//
// This is how a struct offset gets settled without guessing. A global that holds a pointer to a
// game object has exactly one or two writers, and the writer is the constructor, so the `lea`
// that feeds the write states where the member sits inside the object. That is what pinned
// BSGraphics::Renderer::Data: SE writes `lea rbx,[rcx+0x10]` into its renderer global, VR writes
// `lea rbx,[rcx+0x18]` into the same global, and the client had been using the SE offset.
//
// Not a disassembler. It recognises a fixed set of encodings, listed below, which is enough to
// find who touches a global. Follow up with inspect.mjs to read the surrounding bytes.
//
//   node Tools/vr_addresses/xrefs.mjs vr 0x14317E790
//   node Tools/vr_addresses/xrefs.mjs se 0x143286A08 --write

import { openImage, SE_PATH, VR_PATH } from './pe.mjs';

const [which, addrArg, ...flags] = process.argv.slice(2);
if (!which || !addrArg) {
  console.error('usage: xrefs.mjs <se|vr> <address> [--write|--read|--lea]');
  process.exit(2);
}

const REGS = ['ax', 'cx', 'dx', 'bx', 'sp', 'bp', 'si', 'di', 'r8', 'r9', 'r10', 'r11', 'r12', 'r13', 'r14', 'r15'];
const regName = (i, wide) => (i < 8 ? (wide ? 'r' : 'e') + REGS[i] : REGS[i] + (wide ? '' : 'd'));

// opcode -> [mnemonic, immediate bytes, kind]. All of these take a modrm, and the [rip+disp32]
// form is mod=00 rm=101, so the instruction length is rex + opcode + modrm + disp32 + immediate.
const OPS = {
  0x88: ['mov byte [rip], r8', 0, 'write'],
  0x89: ['mov [rip], reg', 0, 'write'],
  0x8a: ['mov r8, byte [rip]', 0, 'read'],
  0x8b: ['mov reg, [rip]', 0, 'read'],
  0x8d: ['lea reg, [rip]', 0, 'lea'],
  0x01: ['add [rip], reg', 0, 'write'],
  0x29: ['sub [rip], reg', 0, 'write'],
  0x39: ['cmp [rip], reg', 0, 'read'],
  0x3b: ['cmp reg, [rip]', 0, 'read'],
  0x85: ['test [rip], reg', 0, 'read'],
  0x80: ['<grp1> byte [rip], imm8', 1, 'read'],
  0x83: ['<grp1> [rip], imm8', 1, 'read'],
  0x81: ['<grp1> [rip], imm32', 4, 'read'],
  0xc6: ['mov byte [rip], imm8', 1, 'write'],
  0xc7: ['mov [rip], imm32', 4, 'write'],
  0xff: ['<grp5> [rip]', 0, 'read'],
};
// group 1 and group 5 name the operation in the modrm reg field rather than the opcode
const GRP1 = ['add', 'or', 'adc', 'sbb', 'and', 'sub', 'xor', 'cmp'];
const GRP5 = ['inc', 'dec', 'call', 'callf', 'jmp', 'jmpf', 'push', '?'];

const img = openImage(which === 'se' ? SE_PATH : VR_PATH);
const want = img.norm(Number(addrArg));
const kinds = flags.length ? flags.map(f => f.replace(/^--/, '')) : null;

let found = 0;
for (const s of img.sections) {
  if (!(s.characteristics & 0x20000000)) continue;
  const buf = img.buf, base = s.rawOffset;
  const n = Math.min(s.rawSize, buf.length - base);

  for (let i = 0; i < n - 12; i++) {
    let p = base + i;
    const rex = buf[p] >= 0x40 && buf[p] <= 0x4f ? buf[p++] : 0;
    const op = OPS[buf[p]];
    if (!op) continue;
    const opcode = buf[p++];
    const modrm = buf[p++];
    if ((modrm >> 6) !== 0 || (modrm & 7) !== 5) continue;

    let [text, immBytes, kind] = op;
    const len = p + 4 + immBytes - (base + i);
    const rva = s.virtualAddress + i;
    if (rva + len + buf.readInt32LE(p) !== want) continue;

    // group 5 /2 and /4 are indirect calls and jumps through the slot, not a read of it
    const sub = (modrm >> 3) & 7;
    if (opcode === 0x83 || opcode === 0x81 || opcode === 0x80) {
      text = text.replace('<grp1>', GRP1[sub]);
      if (GRP1[sub] !== 'cmp') kind = 'write';
    }
    if (opcode === 0xff) {
      text = text.replace('<grp5>', GRP5[sub]);
      kind = sub === 2 || sub === 4 ? 'call' : 'write';
    }
    if (kinds && !kinds.includes(kind)) continue;

    const wide = (rex & 8) !== 0;
    text = text.replace('reg', regName(sub + ((rex & 4) ? 8 : 0), wide));
    const imm = immBytes === 1 ? buf.readInt8(p + 4) : immBytes === 4 ? buf.readInt32LE(p + 4) : null;
    if (imm !== null) text = text.replace(/imm\d+/, `0x${(imm >>> 0).toString(16)}`);

    console.log(`  0x${(img.imageBase + rva).toString(16)}  ${kind.padEnd(5)}  ${text}`);
    found++;
    i += len - 1;
  }
}

console.log(`${found} reference(s) to 0x${(img.imageBase + want).toString(16)} in ${img.path}`);
