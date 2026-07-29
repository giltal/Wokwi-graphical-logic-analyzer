/*
 * amalgamate.js - flatten the chip sources into a single .c file.
 *
 * wokwi.com (the web editor) compiles custom chips from source, one C file per
 * chip: there is no way to add extra translation units and no way to upload a
 * prebuilt .wasm. This produces dist/logic-scope.chip.c, a single file that can
 * be pasted straight into a web project's custom chip.
 *
 *   node tools/amalgamate.js
 *
 * `#include "wokwi-api.h"` is left alone - the web compiler supplies it - and
 * every other local header/source is inlined once, in dependency order.
 */
const fs = require('fs');
const path = require('path');

const SRC = path.join(__dirname, '..', 'src');
const OUT = path.join(__dirname, '..', 'dist', 'logic-scope.chip.c');
const KEEP = new Set(['wokwi-api.h']);

const ROOTS = [
  'capture.c',
  'render.c',
  'decoders/decoder.c',
  'decoders/uart.c',
  'decoders/i2c.c',
  'decoders/spi.c',
  'main.c',
];

const seen = new Set();
const out = [];

function inline(relPath) {
  const key = path.normalize(relPath).replace(/\\/g, '/');
  if (seen.has(key)) {
    return;
  }
  seen.add(key);

  const full = path.join(SRC, key);
  const dir = path.dirname(key);
  out.push(`/* ==== ${key} ${'='.repeat(Math.max(0, 60 - key.length))} */`);

  for (const line of fs.readFileSync(full, 'utf8').split(/\r?\n/)) {
    const m = /^\s*#\s*include\s+"([^"]+)"/.exec(line);
    if (!m || KEEP.has(path.basename(m[1]))) {
      out.push(line);
      continue;
    }
    const target = path.join(dir, m[1]).replace(/\\/g, '/');
    if (!fs.existsSync(path.join(SRC, target))) {
      throw new Error(`${key}: cannot resolve #include "${m[1]}"`);
    }
    inline(target);
  }
  out.push('');
}

out.push('/*');
out.push(' * chip-logic-scope - 8-channel graphical logic analyzer for Wokwi.');
out.push(' *');
out.push(' * GENERATED FILE - do not edit. Built from the sources in chip/src by');
out.push(' * tools/amalgamate.js, so that the chip can be pasted into a wokwi.com');
out.push(' * project, which compiles exactly one C file per custom chip.');
out.push(' */');
out.push('');

for (const root of ROOTS) {
  inline(root);
}

fs.mkdirSync(path.dirname(OUT), { recursive: true });
const text = out.join('\n');
fs.writeFileSync(OUT, text);
console.log(
  `wrote dist/logic-scope.chip.c (${text.length} bytes, ${seen.size} files)`
);
