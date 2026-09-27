// Resolves the Anniversary Edition (1.6.x) Address Library ids used by the client to
// SkyrimVR 1.4.15 offsets, and generates Code/client/VRAddressMap.inl.
//
// The client addresses everything by AE id. The community VR address library is keyed on
// SSE 1.5.97 ids, a different namespace, so there is no direct substitution. The chain is:
//
//   AE id --offsets-1-6-318-0--> AE addr --se-ae-attempted-match--> 1.5.97 addr --+
//   AE id --se_ae--> SSE id --offsets-1-5-97-0--> 1.5.97 addr ------------------- +
//                                                                                |
//   1.5.97 addr --database.csv (human verified)--> VR addr   <-- preferred       |
//   1.5.97 addr --sse_vr.csv (automated diff)----> VR addr   <-- fallback  <-----+
//
// Both paths to the 1.5.97 address are computed; agreement raises confidence and
// disagreement is reported. overrides.csv always wins, so a hand-verified address can
// never be clobbered by a database refresh.
//
// Usage:  node Tools/vr_addresses/resolve.mjs [--fetch]
//   --fetch  re-download the mapping databases even if already cached

import fs from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const REPO = path.resolve(HERE, '..', '..');
const CACHE = path.join(HERE, 'cache');
const OVERRIDES = path.join(HERE, 'overrides.csv');
const OUT_INL = path.join(REPO, 'Code', 'client', 'VRAddressMap.inl');
const OUT_REPORT = path.join(HERE, 'report.tsv');

const BASE_ADDR = 0x140000000;
const SOURCE = 'https://raw.githubusercontent.com/alandtse/skyrim_vr_address_library/main';
const DATABASES = [
  'offsets-1-6-318-0.csv',      // aeid  -> AE addr
  'se_ae.csv',                  // sseid -> aeid
  'offsets-1-5-97-0.csv',       // sseid -> 1.5.97 addr
  'se-ae-attempted-match.csv',  // 1.5.97 addr -> AE addr
  'database.csv',               // sseid, 1.5.97 addr, VR addr, status  (human curated)
  'sse_vr.csv',                 // 1.5.97 addr -> VR addr              (automated diff)
  'addrlib.csv',                // VR addr, 1.5.97 addr, sseid         (last resort)
];

// confidence, highest wins
const CONF = { OVERRIDE: 5, CURATED_IDENTICAL: 4, CURATED_VERIFIED: 3, CURATED_WEAK: 2, AUTO_DIFF: 1, ADDRLIB: 0 };
const CONF_NAME = ['addrlib', 'auto-diff', 'curated-weak', 'curated-verified', 'curated-identical', 'override'];

// ---------------------------------------------------------------- helpers

const hex = s => parseInt(s.trim().replace(/^0x/i, ''), 16);

// Iterates data lines (header skipped) without building a per-file array.
function forEachLine(file, cb) {
  const text = fs.readFileSync(file, 'latin1');
  let i = text.indexOf('\n') + 1;
  while (i < text.length) {
    let nl = text.indexOf('\n', i);
    if (nl < 0) nl = text.length;
    let end = nl;
    if (end > i && text.charCodeAt(end - 1) === 13) end--;
    if (end > i) cb(text.slice(i, end));
    i = nl + 1;
  }
}

async function ensureCache(force) {
  fs.mkdirSync(CACHE, { recursive: true });
  for (const name of DATABASES) {
    const dest = path.join(CACHE, name);
    if (!force && fs.existsSync(dest) && fs.statSync(dest).size > 0) continue;
    process.stdout.write(`fetching ${name} ... `);
    const res = await fetch(`${SOURCE}/${name}`);
    if (!res.ok) throw new Error(`${name}: HTTP ${res.status}`);
    fs.writeFileSync(dest, Buffer.from(await res.arrayBuffer()));
    console.log(`${(fs.statSync(dest).size / 1e6).toFixed(1)} MB`);
  }
}

const db = name => path.join(CACHE, name);

// ---------------------------------------------------------------- 1. scan the client

function scanCodebase() {
  const found = new Map(); // aeid -> {name, kind, sites[]}
  const record = (id, name, kind, site) => {
    let e = found.get(id);
    if (!e) found.set(id, (e = { name, kind, sites: [] }));
    e.sites.push(site);
  };

  const walk = dir => {
    for (const ent of fs.readdirSync(dir, { withFileTypes: true })) {
      const p = path.join(dir, ent.name);
      if (ent.isDirectory()) {
        if (!['node_modules', 'dist', '.angular', 'build'].includes(ent.name)) walk(p);
      } else if (/\.(cpp|h|inl)$/.test(ent.name)) {
        const rel = path.relative(REPO, p).replace(/\\/g, '/');
        fs.readFileSync(p, 'utf8').split('\n').forEach((line, idx) => {
          const site = `${rel}:${idx + 1}`;
          let m = /POINTER_SKYRIMSE\s*\((.+)\)\s*;/.exec(line);
          if (m) {
            const args = m[1].split(',').map(s => s.trim());
            const id = args.at(-1);
            if (/^\d+$/.test(id)) record(+id, args.at(-2), 'pointer', site);
            return;
          }
          // the name may be qualified, as it is when an extern declared in a header is defined at
          // namespace scope: `const VersionDbPtr<T> internal::DynamicCast(109689);`
          m = /VersionDbPtr\s*<.+?>\s*((?:[A-Za-z_]\w*::)*[A-Za-z_]\w*)\s*\(\s*(\d+)\s*\)/.exec(line);
          if (m) return record(+m[2], m[1].split('::').at(-1), 'hook', site);
          m = /internal::RttiLocator<(\w+)>\s+registerRtti_\w+\s*\(\s*(\d+)\s*\)/.exec(line);
          if (m) return record(+m[2], m[1], 'rtti', site);
        });
      }
    }
  };
  walk(path.join(REPO, 'Code', 'client'));
  return found;
}

// ---------------------------------------------------------------- 2. resolve

function resolve(need) {
  const ids = new Set(need.keys());

  // pass 1: AE id -> AE address
  const aeAddr = new Map();
  forEachLine(db('offsets-1-6-318-0.csv'), l => {
    const c = l.indexOf(',');
    const id = +l.slice(0, c);
    if (ids.has(id)) aeAddr.set(id, hex(l.slice(c + 1)));
  });

  // pass 2: AE id -> SSE id
  const sseId = new Map();
  forEachLine(db('se_ae.csv'), l => {
    const f = l.split(',');
    const ae = +f[1];
    if (ids.has(ae) && +f[0]) sseId.set(ae, +f[0]);
  });

  // pass 3: SSE id -> 1.5.97 address
  const wantSseId = new Set(sseId.values());
  const sseIdAddr = new Map();
  forEachLine(db('offsets-1-5-97-0.csv'), l => {
    const c = l.indexOf(',');
    const id = +l.slice(0, c);
    if (wantSseId.has(id)) sseIdAddr.set(id, hex(l.slice(c + 1)));
  });

  // pass 4: AE address -> 1.5.97 address (reverse of the attempted match)
  const wantAeAddr = new Set(aeAddr.values());
  const byAeAddr = new Map();
  forEachLine(db('se-ae-attempted-match.csv'), l => {
    const c = l.indexOf(',');
    const ae = hex(l.slice(c + 1));
    if (wantAeAddr.has(ae) && !byAeAddr.has(ae)) byAeAddr.set(ae, hex(l.slice(0, c)));
  });

  // combine the two independent routes to a 1.5.97 address
  const pick = new Map(); // aeid -> {sse, route}
  for (const id of ids) {
    const viaAddr = byAeAddr.get(aeAddr.get(id));
    const viaId = sseIdAddr.get(sseId.get(id));
    if (viaAddr && viaId) pick.set(id, { sse: viaAddr, route: viaAddr === viaId ? 'both' : 'conflict' });
    else if (viaAddr) pick.set(id, { sse: viaAddr, route: 'addr-match' });
    else if (viaId) pick.set(id, { sse: viaId, route: 'id-map' });
  }

  // pass 5-7: 1.5.97 address -> VR address
  const wantSse = new Set([...pick.values()].map(p => p.sse));
  const curated = new Map(); // sse addr -> {vr, conf, name}
  for (const l of fs.readFileSync(db('database.csv'), 'utf8').split('\n').slice(1)) {
    if (!l.trim()) continue;
    const f = l.replace(/\r$/, '').split(',');
    const sse = hex(f[1]), vr = hex(f[2]), status = +f[3] || 0;
    if (!wantSse.has(sse) || !vr) continue;
    const conf = status >= 4 ? CONF.CURATED_IDENTICAL : status === 3 ? CONF.CURATED_VERIFIED : CONF.CURATED_WEAK;
    const prev = curated.get(sse);
    if (!prev || conf > prev.conf) curated.set(sse, { vr, conf, name: (f[4] || '').replace(/^"|"$/g, '').trim() });
  }
  const autoDiff = new Map();
  forEachLine(db('sse_vr.csv'), l => {
    const c = l.indexOf(',');
    const sse = hex(l.slice(0, c));
    if (wantSse.has(sse)) autoDiff.set(sse, hex(l.slice(c + 1)));
  });
  const addrlib = new Map();
  forEachLine(db('addrlib.csv'), l => {
    const f = l.split(',');
    const sse = hex(f[1]);
    if (wantSse.has(sse)) addrlib.set(sse, hex(f[0]));
  });

  // assemble
  const out = new Map();
  for (const [id, info] of need) {
    const p = pick.get(id);
    const row = { id, ...info, sse: p?.sse, route: p?.route ?? 'none', flags: [] };
    if (p?.route === 'conflict') row.flags.push('SSE_ROUTE_CONFLICT');
    if (p) {
      const cur = curated.get(p.sse), auto = autoDiff.get(p.sse), alib = addrlib.get(p.sse);
      if (cur) { row.vr = cur.vr; row.conf = cur.conf; row.dbName = cur.name; }
      else if (auto) { row.vr = auto; row.conf = CONF.AUTO_DIFF; }
      else if (alib) { row.vr = alib; row.conf = CONF.ADDRLIB; }
      if (cur && auto && cur.vr !== auto) row.flags.push('VR_SOURCE_CONFLICT');
    }
    out.set(id, row);
  }
  return out;
}

// ---------------------------------------------------------------- 3. overrides

function applyOverrides(rows) {
  if (!fs.existsSync(OVERRIDES)) return 0;
  let n = 0;
  fs.readFileSync(OVERRIDES, 'utf8').split('\n').forEach(line => {
    const s = line.trim();
    if (!s || s.startsWith('#')) return;
    const [idStr, vrStr, ...rest] = s.split(',');
    const id = +idStr, vr = hex(vrStr);
    if (!Number.isFinite(id) || !Number.isFinite(vr)) return;
    const row = rows.get(id);
    if (!row) {
      console.warn(`  override for ${id} is not used anywhere in the client, ignoring`);
      return;
    }
    row.vr = vr;
    row.conf = CONF.OVERRIDE;
    row.flags = row.flags.filter(f => f !== 'VR_SOURCE_CONFLICT' && f !== 'SSE_ROUTE_CONFLICT');
    row.note = rest.join(',').trim();
    n++;
  });
  return n;
}

// ---------------------------------------------------------------- 4. emit

function emit(rows) {
  const sorted = [...rows.values()].sort((a, b) => a.id - b.id);
  const mapped = sorted.filter(r => r.vr);

  for (const r of mapped) {
    if (r.vr < BASE_ADDR) throw new Error(`id ${r.id}: VR address 0x${r.vr.toString(16)} is below the image base`);
  }

  const counts = {};
  for (const r of sorted) {
    const k = r.vr ? CONF_NAME[r.conf] : 'UNMAPPED';
    counts[k] = (counts[k] || 0) + 1;
  }

  const head = [
    '// Generated by Tools/vr_addresses/resolve.mjs. Do not edit by hand.',
    '// Hand-verified corrections belong in Tools/vr_addresses/overrides.csv.',
    '//',
    `// AE ids used by the client: ${sorted.length}, mapped to SkyrimVR 1.4.15: ${mapped.length}`,
    ...Object.entries(counts).sort((a, b) => b[1] - a[1]).map(([k, v]) => `//   ${k.padEnd(18)} ${v}`),
    '//',
    '// vrOffset is an RVA. A zero offset means the id is known but has no VR mapping yet;',
    '// requesting one is a hard error at runtime, never a silent nullptr.',
    '',
    '// clang-format off',
    'static constexpr VRAddress kVRAddressMap[] = {',
  ];
  const body = sorted.map(r => {
    const off = r.vr ? r.vr - BASE_ADDR : 0;
    return `    {${r.id}, 0x${off.toString(16).toUpperCase().padStart(7, '0')}, ${r.vr ? r.conf : 0}, "${r.name}"},`;
  });
  fs.writeFileSync(OUT_INL, [...head, ...body, '};', '// clang-format on', ''].join('\n'));

  const cols = ['aeid', 'kind', 'name', 'confidence', 'vr', 'vr_offset', 'sse_1_5_97', 'sse_route', 'flags', 'db_name', 'site'];
  const tsv = sorted.map(r => [
    r.id, r.kind, r.name, r.vr ? CONF_NAME[r.conf] : 'UNMAPPED',
    r.vr ? '0x' + r.vr.toString(16) : '', r.vr ? '0x' + (r.vr - BASE_ADDR).toString(16) : '',
    r.sse ? '0x' + r.sse.toString(16) : '', r.route, r.flags.join('|'), r.dbName || r.note || '', r.sites[0],
  ].join('\t'));
  fs.writeFileSync(OUT_REPORT, [cols.join('\t'), ...tsv].join('\n') + '\n');

  return { sorted, mapped, counts };
}

// ---------------------------------------------------------------- main

const force = process.argv.includes('--fetch');
await ensureCache(force);

const need = scanCodebase();
console.log(`\nAE ids referenced by the client: ${need.size}`);
for (const kind of ['hook', 'pointer', 'rtti']) {
  console.log(`  ${kind.padEnd(8)} ${[...need.values()].filter(r => r.kind === kind).length}`);
}

const rows = resolve(need);
const overridden = applyOverrides(rows);
const { sorted, mapped, counts } = emit(rows);

console.log(`\nresolved ${mapped.length}/${sorted.length} (${(100 * mapped.length / sorted.length).toFixed(1)}%), ${overridden} from overrides.csv`);
for (const [k, v] of Object.entries(counts).sort((a, b) => b[1] - a[1])) console.log(`  ${k.padEnd(18)} ${v}`);

const flagged = sorted.filter(r => r.flags.length);
const unmapped = sorted.filter(r => !r.vr);

if (flagged.length) {
  console.log(`\nneeds manual review (${flagged.length}):`);
  for (const r of flagged) console.log(`  ${String(r.id).padEnd(7)} ${r.kind.padEnd(8)} ${r.name.padEnd(28)} ${r.flags.join(',')}`);
}
if (unmapped.length) {
  console.log(`\nno VR mapping (${unmapped.length}):`);
  for (const r of unmapped) console.log(`  ${String(r.id).padEnd(7)} ${r.kind.padEnd(8)} ${r.name.padEnd(28)} route=${r.route}  ${r.sites[0]}`);
}
console.log(`\nwrote ${path.relative(REPO, OUT_INL)} and ${path.relative(REPO, OUT_REPORT)}`);
