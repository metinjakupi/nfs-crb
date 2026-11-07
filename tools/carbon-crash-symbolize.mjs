#!/usr/bin/env node
// carbon-crash-symbolize.mjs - names the wasm frames of a player's report.
//
// A report's "Crash location" lists the crashed thread's frames as "wasm N 0xOFFSET name";
// when the release did not publish CarbonRecomp.js.symbols the name is "?". This reads the
// build's own symbols file (emcc --emit-symbol-map: one "N:name" line per wasm function; keep
// it with every published build, it is beside the build's CarbonRecomp.wasm) and prints the
// report with the names filled in. Raw browser stacks (wasm-function[N]) are named as well.
//
//   node tools/carbon-crash-symbolize.mjs REPORT.txt path/to/CarbonRecomp.js.symbols
//
// Check that the symbols belong to the report's "Runtime wasm SHA-256" before trusting names.
import { readFileSync } from "node:fs";

export function parseSymbols(text) {
  const names = new Map();
  for (const line of text.split("\n")) {
    const i = line.indexOf(":");
    if (i > 0 && /^\d+$/.test(line.slice(0, i))) names.set(Number(line.slice(0, i)), line.slice(i + 1).trim());
  }
  return names;
}

export function symbolize(report, names) {
  return report
    .replace(/(\bwasm (\d+)(?: 0x[0-9a-f]+)? )\?(?=\s|$)/gim, (all, head, n) => head + (names.get(Number(n)) || "?"))
    .replace(/wasm-function\[(\d+)\](?::0x[0-9a-f]+)?/gi, (all, n) => names.has(Number(n)) ? `${all} <${names.get(Number(n))}>` : all);
}

if (process.argv[1] && import.meta.url.endsWith(process.argv[1].split("/").pop())) {
  const [report, symbols] = process.argv.slice(2);
  if (!report || !symbols) {
    console.error("usage: node tools/carbon-crash-symbolize.mjs REPORT.txt CarbonRecomp.js.symbols");
    process.exit(2);
  }
  process.stdout.write(symbolize(readFileSync(report, "utf8"), parseSymbols(readFileSync(symbols, "utf8"))));
}
