#!/usr/bin/env node
// Minify the staged site only. The source tree stays readable.
import { readdir, readFile, writeFile } from 'node:fs/promises';
import { createRequire } from 'node:module';
import { resolve, relative, join } from 'node:path';
import { gzipSync } from 'node:zlib';

const require = createRequire(new URL('../emsdk/upstream/emscripten/package.json', import.meta.url));
const { minify } = require('terser');
const { version } = require('terser/package.json');
const root = process.argv[2];
if (!root || process.argv.length !== 3) {
  throw new Error('Usage: node tools/carbon-minify-js.mjs STAGED_SITE_DIRECTORY');
}
const directory = resolve(root);
async function scripts(dir) {
  const found = [];
  for (const entry of await readdir(dir, { withFileTypes: true })) {
    const path = join(dir, entry.name);
    if (entry.isSymbolicLink()) throw new Error(`Refusing symbolic link: ${path}`);
    if (entry.isDirectory()) found.push(...await scripts(path));
    else if (entry.isFile() && entry.name.endsWith('.js')) found.push(path);
  }
  return found.sort();
}
const files = await scripts(directory);
if (!files.length) throw new Error(`No JavaScript files in ${directory}`);
const compressedSize = text => gzipSync(text, { level: 9 }).length;
const prepared = [];
// Parse every file before changing any of them. Keep names, exports and scopes:
// net.js and lan.js turn function source into standalone worker scripts.
for (const file of files) {
  const original = await readFile(file, 'utf8');
  const options = {
    compress: false,
    mangle: false,
    format: { ecma: 2020, comments: 'some' }, // Retain copyright/license/preserve comments.
  };
  let result;
  try {
    result = await minify({ [relative(directory, file)]: original }, options);
  } catch (error) {
    // The launcher uses top-level await. Parse it as a module, without enabling
    // the compression/mangling that could change classic script globals.
    if (error.name !== 'SyntaxError') throw error;
    result = await minify({ [relative(directory, file)]: original }, { ...options, module: true });
  }
  const candidate = result.code + '\n';
  const smaller = Buffer.byteLength(candidate) < Buffer.byteLength(original)
    && compressedSize(candidate) <= compressedSize(original);
  const output = smaller ? candidate : original;
  prepared.push({ file, output, changed: output !== original, stats: {
    path: relative(directory, file),
    before: Buffer.byteLength(original), after: Buffer.byteLength(output),
    gzipBefore: compressedSize(original), gzipAfter: compressedSize(output),
  } });
}
for (const item of prepared) {
  if (item.changed) await writeFile(item.file, item.output);
}
const totals = { before: 0, after: 0, gzipBefore: 0, gzipAfter: 0 };
for (const item of prepared) {
  for (const key of Object.keys(totals)) totals[key] += item.stats[key];
}
console.log(JSON.stringify({ terser: version, files: prepared.map(item => item.stats), totals }, null, 2));
