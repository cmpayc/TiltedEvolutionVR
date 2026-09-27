// Derives the startup-path addresses (winMain, MainInit, cMainLoop) from the binaries instead
// of mapping them. All three are missing from the id chain because Anniversary Edition
// restructured them, but they can be read off directly.
//
// The anchor is the CRT. __scrt_common_main_seh calls WinMain immediately after
// _get_narrow_winmain_command_line, an import that exists unchanged in both exes, so the game's
// WinMain is wherever that call sequence points. From there WinMain's own calls line up one for
// one between the builds, and the main loop is reached through a Sleep(50) block that is
// byte-identical apart from displacements.
//
// SE is resolved the same way and checked against the 1.6.1170 versionlib, so the method proves
// itself on the side where the answer is already known before it is trusted on VR.
//
// Usage:  node Tools/vr_addresses/startup.mjs

import fs from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';
import { openImage, SE_PATH, VR_PATH, HAVE_UNPACKED } from './pe.mjs';
import { loadVersionLib } from './versionlib.mjs';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const SE_GAME = 'C:/Program Files (x86)/Steam/steamapps/common/Skyrim Special Edition';

if (!HAVE_UNPACKED) {
  console.error('needs unpacked binaries in cache/ (see the note in pe.mjs)');
  process.exit(2);
}

// ---------------------------------------------------------------- .text scanning helpers

function textOf(img) {
  const sec = img.sections.find(s => s.name === '.text');
  return { sec, from: sec.rawOffset, to: sec.rawOffset + Math.min(sec.rawSize, sec.virtualSize) };
}

const rvaAt = (img, off) => {
  const { sec, from } = textOf(img);
  return sec.virtualAddress + (off - from);
};

// every offset in .text where a masked pattern matches; -1 in the pattern is a wildcard byte
function* scan(img, pattern) {
  const { from, to } = textOf(img);
  outer: for (let o = from; o < to - pattern.length; o++) {
    for (let i = 0; i < pattern.length; i++) if (pattern[i] >= 0 && img.buf[o + i] !== pattern[i]) continue outer;
    yield o;
  }
}

const bytes = s => s.trim().split(/\s+/).map(t => (t === '?' ? -1 : parseInt(t, 16)));

// target of the E8/E9 rel32 whose opcode sits at this .text offset
const relTarget = (img, off) => rvaAt(img, off) + 5 + img.buf.readInt32LE(off + 1);

// ---------------------------------------------------------------- the walk

// __scrt_common_main_seh:  call _get_narrow_winmain_command_line
//                          mov r8, rax / mov r9d, ebx / xor edx, edx / lea rcx, [__ImageBase]
//                          call WinMain
const INVOKE_MAIN = bytes('e8 ? ? ? ? 4c 8b c0 44 8b cb 33 d2 48 8d 0d ? ? ? ? e8');

function findWinMain(img) {
  // the import is reached through a "jmp [rip+iat]" thunk, so find the thunk first
  const slot = [...img.imports()].find(([, n]) => n.endsWith('!_get_narrow_winmain_command_line'))?.[0];
  if (slot === undefined) throw new Error(`${img.path}: no _get_narrow_winmain_command_line import`);

  let thunk;
  for (const o of scan(img, bytes('ff 25 ? ? ? ?'))) {
    if (rvaAt(img, o) + 6 + img.buf.readInt32LE(o + 2) === slot) { thunk = rvaAt(img, o); break; }
  }
  if (thunk === undefined) throw new Error(`${img.path}: no thunk for the import`);

  const found = new Set();
  for (const o of scan(img, INVOKE_MAIN)) {
    if (relTarget(img, o) !== thunk) continue;
    found.add(relTarget(img, o + INVOKE_MAIN.length - 1));
  }
  if (found.size !== 1) throw new Error(`${img.path}: ${found.size} candidates for WinMain`);
  return { thunk, winMain: [...found][0] };
}

// WinMain:  sub rsp,N / mov [rip+X],rcx / mov [rip+Y],r8 / call A / test al,al / je
//           call B / test al,al / je / movzx ecx,byte [rip+Z] / call C
// The je is short in VR and near in SE, so the two halves are matched separately.
function winMainCalls(img, winMain) {
  const { sec, from } = textOf(img);
  const at = from + (winMain - sec.virtualAddress);
  const head = bytes('48 89 0d ? ? ? ? 4c 89 05 ? ? ? ? e8');
  let i = 0;
  while (i < 16 && !head.every((b, k) => b < 0 || img.buf[at + i + k] === b)) i++;
  if (i === 16) throw new Error(`${img.path}: WinMain does not open with the expected stores`);

  const a = at + i + head.length - 1;
  const calls = [relTarget(img, a)];
  // step over "test al,al" + a short or near je, then the next call, twice
  let p = a + 5;
  for (let n = 0; n < 2; n++) {
    if (img.buf[p] !== 0x84 || img.buf[p + 1] !== 0xc0) break;
    p += 2;
    p += img.buf[p] === 0x0f ? 6 : 2;
    if (img.buf[p] === 0x0f && img.buf[p + 1] === 0xb6) p += 7; // movzx ecx, byte [rip+Z]
    if (img.buf[p] !== 0xe8) break;
    calls.push(relTarget(img, p));
    p += 5;
  }
  return calls;
}

// The block around the main loop call, identical in both builds apart from displacements:
//   mov ecx,50 / call [Sleep] / jmp +0x34 / mov rcx,[rip+Main] / call cMainLoop / test bl,bl / je
const MAIN_LOOP = bytes('b9 32 00 00 00 ff 15 ? ? ? ? eb 34 48 8b 0d ? ? ? ? e8 ? ? ? ? 84 db 74 24');

function findMainLoop(img) {
  const found = new Set();
  for (const o of scan(img, MAIN_LOOP)) found.add(relTarget(img, o + 20));
  if (found.size !== 1) throw new Error(`${img.path}: ${found.size} candidates for the main loop`);
  return [...found][0];
}

// ---------------------------------------------------------------- run

const abs = (img, rva) => '0x' + (rva + img.imageBase).toString(16).toUpperCase();

function walk(img) {
  const { thunk, winMain } = findWinMain(img);
  const calls = winMainCalls(img, winMain);
  const mainLoop = findMainLoop(img);
  console.log(`\n${img.path}`);
  console.log(`  _get_narrow_winmain_command_line thunk  ${abs(img, thunk)}`);
  console.log(`  winMain                                 ${abs(img, winMain)}`);
  calls.forEach((c, i) => console.log(`    call ${'ABC'[i]} from WinMain                     ${abs(img, c)}${i === 1 ? '   <- MainInit' : ''}`));
  console.log(`  cMainLoop                               ${abs(img, mainLoop)}`);
  return { winMain, mainInit: calls[1], mainLoop };
}

const se = openImage(SE_PATH);
const vr = openImage(VR_PATH);
const seFound = walk(se);
const vrFound = walk(vr);

// SE is the control: the versionlib already knows these three, so a mismatch means the walk is
// wrong and the VR answers cannot be trusted either.
const lib = loadVersionLib(path.join(SE_GAME, 'Data/SKSE/Plugins/versionlib-1-6-1170-0.bin'));
const EXPECT = [
  [36544, 'winMain', seFound.winMain, vrFound.winMain],
  [36548, 'MainInit', seFound.mainInit, vrFound.mainInit],
  [36564, 'cMainLoop', seFound.mainLoop, vrFound.mainLoop],
];

console.log(`\nSE control against versionlib ${lib.version}`);
let bad = 0;
for (const [id, name, seRva] of EXPECT) {
  const want = lib.offsets.get(id);
  const ok = want === seRva;
  if (!ok) bad++;
  console.log(`  ${String(id).padEnd(7)} ${name.padEnd(10)} walk ${abs(se, seRva)}  versionlib ${want === undefined ? 'missing' : abs(se, want)}  ${ok ? 'ok' : 'MISMATCH'}`);
}

if (bad) {
  console.error('\nthe walk disagrees with the SE versionlib, do not use these VR addresses');
  process.exit(1);
}

console.log('\noverrides.csv lines:');
for (const [id, name, , vrRva] of EXPECT) console.log(`  ${id}, ${abs(vr, vrRva)}, ${name}`);

// keep the recorded overrides honest
const rec = new Map();
for (const l of fs.readFileSync(path.join(HERE, 'overrides.csv'), 'utf8').split('\n')) {
  const s = l.trim();
  if (!s || s.startsWith('#')) continue;
  const [id, addr] = s.split(',');
  rec.set(+id, parseInt(addr.trim(), 16));
}
const drift = EXPECT.filter(([id, , , vrRva]) => rec.get(id) !== vrRva + vr.imageBase);
if (drift.length) {
  console.error(`\noverrides.csv disagrees with this walk for: ${drift.map(d => d[0]).join(', ')}`);
  process.exit(1);
}
console.log('\noverrides.csv matches the walk');
