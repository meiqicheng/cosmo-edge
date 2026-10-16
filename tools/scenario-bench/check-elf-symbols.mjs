// check-elf-symbols.mjs — verify symbols are EXPORTED in .dynsym of an ELF shared object.
// Usage: node check-elf-symbols.mjs <file.so> [symbol ...]
// Exit code 0 = ALL requested symbols exported; 1 = any missing; 2 = parse error.

import fs from 'node:fs';

function die(msg) {
  console.error(msg);
  process.exit(2);
}

const [, , file, ...wanted] = process.argv;
if (!file) die('usage: node check-elf-symbols.mjs <file.so> [symbol ...]');
const buf = fs.readFileSync(file);

if (buf.readUInt32LE(0) !== 0x46_4c_45_7f) die(`not an ELF file: ${file}`);
const is64 = buf.readUInt8(4) === 2;
if (!is64) die('only ELF64 supported');

const e_shoff = Number(buf.readBigUInt64LE(0x28));
const e_shentsize = buf.readUInt16LE(0x3a);
const e_shnum = buf.readUInt16LE(0x3c);

const sections = [];
for (let i = 0; i < e_shnum; i++) {
  const off = e_shoff + i * e_shentsize;
  sections.push({
    nameOff: buf.readUInt32LE(off),
    type: buf.readUInt32LE(off + 4),
    offset: Number(buf.readBigUInt64LE(off + 0x18)),
    size: Number(buf.readBigUInt64LE(off + 0x20)),
    link: buf.readUInt32LE(off + 0x28),
    entsize: Number(buf.readBigUInt64LE(off + 0x38)),
  });
}

const exported = new Set();
for (const sec of sections) {
  if (sec.type !== 11 /* SHT_DYNSYM */) continue;
  const strSec = sections[sec.link];
  const strTab = buf.subarray(strSec.offset, strSec.offset + strSec.size);
  const count = Math.floor(sec.size / 24);
  for (let i = 0; i < count; i++) {
    const symOff = sec.offset + i * 24;
    const nameOff = buf.readUInt32LE(symOff);
    const end = strTab.indexOf(0, nameOff);
    if (nameOff < end && end !== -1) exported.add(strTab.toString('ascii', nameOff, end));
  }
}

if (wanted.length === 0) {
  console.log(`${file}: ${exported.size} dynamic symbols`);
  process.exit(0);
}
const missing = wanted.filter((s) => !exported.has(s));
console.log(`${file}: missing=[${missing.join(', ') || 'NONE'}]`);
process.exit(missing.length ? 1 : 0);
