// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// hosting-values-check.mjs — the committed hosting headers held, value by value, to what omx-contract's reader reads.
//
//   node hosting-values-check.mjs <whole-read json> <src dir>
//   node hosting-values-check.mjs --self-test <whole-read json> <src dir>
//
// <whole-read json> is `omx-contract render --target json` of the contract directory that holds omx-clap-core's
// package: HOSTED_STAGE_LIMITS as the reader parsed it, not as a render spelled it. Each field of HOSTED_STAGE_LIMITS
// must stand in <src dir>/hosted_stage_limits.h as OMX_HOSTED_STAGE_<FIELD> with its value, and each field but the
// worker ones in clap_host_limits.h as CLAP_HOST_<FIELD>; each code of the package's codes items (HOSTING_IDLE_CODES,
// the dimension this tree owns, #296) as CLAP_HOST_CODE_<NAME> with its string; a define the declaration lacks, or one
// it names that is missing or differs, is a failure. Every other refusal code is omx-contract's
// (tests/hosting-codes-check.sh holds them). The self test changes every value of both files in turn, drops it, and
// adds one the declaration does not name, and each of those must fail the check.
// Exit 0 held; 1 a difference; 2 usage.
import { readFileSync } from 'node:fs';

const snake = (s) => s.replace(/([A-Z])/g, '_$1').toUpperCase();

function expected(read) {
  const items = JSON.parse(read).items;
  const limits = items.HOSTED_STAGE_LIMITS.value;
  const host = new Map();
  const stage = new Map();
  for (const [k, v] of Object.entries(limits)) {
    stage.set(`OMX_HOSTED_STAGE_${snake(k)}`, v);
    if (!k.startsWith('worker')) host.set(`CLAP_HOST_${snake(k)}`, v);
  }
  for (const [name, item] of Object.entries(items)) {
    // this tree's codes items are the ones of the sheet's own file: omx-contract's hosting/*.json are not rendered here
    if (!name.endsWith('_CODES') || item.kind !== 'codes' || item.rel !== items.HOSTED_STAGE_LIMITS.rel) continue;
    for (const [k, c] of Object.entries(item.value)) host.set(`CLAP_HOST_CODE_${snake(k)}`, c.code);
  }
  return { 'clap_host_limits.h': host, 'hosted_stage_limits.h': stage };
}

// the defines of a header: name to its value, a number read as one and a string as the text between its quotes
function defines(text) {
  const out = new Map();
  for (const line of text.split('\n')) {
    const m = /^#define ([A-Z0-9_]+) (?:"([^"]*)"|(-?[0-9.]+)[uf]?)(?:\s|$)/.exec(line);
    if (m) out.set(m[1], m[2] !== undefined ? m[2] : Number(m[3]));
  }
  return out;
}

function check(want, texts) {
  const bad = [];
  for (const [file, names] of Object.entries(want)) {
    const have = defines(texts[file]);
    for (const [n, v] of names) {
      if (!have.has(n)) bad.push(`${file}: ${n} is declared (${JSON.stringify(v)}) and not defined`);
      else if (have.get(n) !== v) bad.push(`${file}: ${n} is ${JSON.stringify(have.get(n))}, declared ${JSON.stringify(v)}`);
    }
    for (const n of have.keys()) {
      if (!names.has(n) && /^(CLAP_HOST_|OMX_HOSTED_STAGE_)/.test(n)) bad.push(`${file}: ${n} is defined and not declared`);
    }
  }
  return bad;
}

const args = process.argv.slice(2);
const self = args[0] === '--self-test';
if (self) args.shift();
if (args.length !== 2) { console.error(readFileSync(new URL(import.meta.url), 'utf8').split('\n')[5].slice(3)); process.exit(2); }
const [readPath, dir] = args;
const want = expected(readFileSync(readPath, 'utf8'));
const texts = {};
for (const f of Object.keys(want)) texts[f] = readFileSync(`${dir}/${f}`, 'utf8');

const bad = check(want, texts);
if (bad.length) { for (const b of bad) console.error(`hosting-values-check: ${b}`); process.exit(1); }
const total = [...Object.values(want)].reduce((a, m) => a + m.size, 0);
if (!self) { console.log(`hosting-values-check: ${total} declared values held by src/clap_host_limits.h and src/hosted_stage_limits.h`); process.exit(0); }

// one sabotage arm per declared value, and one per define the declaration does not name
let arms = 0;
let missed = 0;
const arm = (what, file, fn) => {
  arms++;
  const mutated = { ...texts, [file]: fn(texts[file]) };
  if (mutated[file] === texts[file]) { console.error(`hosting-values-check: self test: ${what} left the file unchanged`); missed++; return; }
  if (check(want, mutated).length === 0) { console.error(`hosting-values-check: self test: ${what} was not caught`); missed++; }
};
for (const [file, names] of Object.entries(want)) {
  for (const [n, v] of names) {
    const re = new RegExp(`^(#define ${n} )(.*)$`, 'm');
    arm(`changing ${n}`, file, (t) => t.replace(re, (_, a, b) => a + (typeof v === 'string' ? '"x' + v + '"' : `${v + 1}u`) + (b.includes('///') ? '  ' + b.slice(b.indexOf('///')) : '')));
    arm(`dropping ${n}`, file, (t) => t.replace(re, ''));
  }
  arm(`adding a define to ${file}`, file, (t) => t.replace('\n#endif', `\n#define ${file.startsWith('hosted') ? 'OMX_HOSTED_STAGE_EXTRA' : 'CLAP_HOST_EXTRA'} 1\n#endif`));
}
if (missed) process.exit(1);
console.log(`hosting-values-check: self test: ${arms} sabotage arms, each caught`);
