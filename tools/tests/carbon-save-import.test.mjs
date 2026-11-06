// The PC save checks in source/web/saves.js, run on files built from scratch here. The builder
// has its own checksum, so a mistake in the page's copy cannot pass by agreeing with itself.
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';
import crypto from 'node:crypto';

const source = fs.readFileSync(new URL('../../source/web/saves.js', import.meta.url), 'utf8');
const block = source.slice(source.indexOf('// SAVE-CHECK-START'), source.indexOf('// SAVE-CHECK-END'));
const page = vm.runInNewContext(`${block};({checkCareer,findCareers,careerForThisGame})`);

function crc(u8, start, end) { // bit by bit, as the table in the game's data works out
  let a = ~((u8[start] << 24 | u8[start + 1] << 16 | u8[start + 2] << 8 | u8[start + 3]) >>> 0);
  for (let i = start + 4; i < end; i++) {
    for (let bit = 7; bit >= 0; bit--) {
      const top = a >>> 31;
      a = ((a << 1) | ((u8[i] >>> bit) & 1)) ^ (top ? 0x04c11db7 : 0);
    }
  }
  return ~a >>> 0;
}
function power(base, e, n) { // the game's signature, worked out with Node's MD5
  let r = 1n; base %= n;
  for (; e; e >>= 1n) { if (e & 1n) r = r * base % n; base = base * base % n; }
  return r;
}
const N = BigInt('0xef620ebb3403d099478fe08aaa365e57fc98e72983fe82cf691328bc47c81de807f34ea0a0a20e969b8ddf94c6dec0b987284be4d4406b7e4a50abe5860b960d');
const E = BigInt('0x2829285589e575f2b149d71f0c6d70a88c4d4ec2ed4d1ebf135ece7fbfff37dce999a2ae905ce2a0aa219daa065152b845208a6e676f4f8bd129d25360bdd0e1');
function sign(u8) {
  let digest = crypto.createHash('md5').update(u8.subarray(0x3c)).digest();
  const parts = [digest];
  for (let i = 0; i < 3; i++) parts.push(digest = crypto.createHash('md5').update(digest).digest());
  const sum = power(BigInt(`0x${Buffer.concat(parts).reverse().toString('hex')}`), E, N);
  u8.set(Buffer.from(sum.toString(16).padStart(128, '0'), 'hex').reverse().subarray(0, 16), 0x2c);
}
function seal(u8) {
  const v = new DataView(u8.buffer);
  if (u8.length === 0x4602c) sign(u8);
  v.setUint32(0x10, crc(u8, 0x1c, 0x2c), true);
  v.setUint32(0x14, crc(u8, 0x2c, u8.length), true);
  v.setUint32(0x18, crc(u8, 0, 0x18), true);
  return u8;
}
function save(edit = () => {}, size = 0x4602c) {
  const u8 = new Uint8Array(size), v = new DataView(u8.buffer);
  v.setUint32(0, 0x4d433032, true); v.setUint32(4, 0x4602c, true); v.setUint32(8, 0x10, true); v.setUint32(12, 0x46000, true);
  v.setUint32(0x1c, 0x1b3c, true); v.setUint32(0x20, 0x34b48, true); v.setUint32(0x24, 6, true); v.setUint32(0x28, 0x5cda943, true);
  v.setUint32(0x2c + 0x50, 0x656d6147, true);
  for (let i = 0x100; i < 0x2000; i++) u8[i] = i * 7;
  edit(v, u8);
  return seal(u8);
}

test('a sound career passes', () => assert.equal(page.checkCareer(save()), null));
test('a career with a good checksum but no good signature is damaged', () => {
  const u8 = save(), v = new DataView(u8.buffer);
  u8[0x500] ^= 1;
  v.setUint32(0x14, crc(u8, 0x2c, u8.length), true);
  v.setUint32(0x18, crc(u8, 0, 0x18), true);
  assert.equal(page.checkCareer(u8), 'corrupt');
});
test('a career written by another serial number is made to fit this game', () => {
  const serial = new TextEncoder().encode('IAMNOTAVALIDSERIALNR');
  const u8 = save((_, b) => b.set(serial, 0x3c));
  assert.equal(page.checkCareer(u8), null);
  const out = page.careerForThisGame(u8);
  assert.notEqual(out, u8);
  assert.equal(page.checkCareer(out), null);
  assert.ok(out.subarray(0x3c, 0x5c).every((b) => !b));
  assert.deepEqual([...out.subarray(0x5c)], [...u8.subarray(0x5c)]);
  assert.deepEqual([...u8.subarray(0x3c, 0x50)], [...serial]);
});
test('a career with no serial number is left as it is', () => {
  const u8 = save();
  assert.equal(page.careerForThisGame(u8), u8);
});
test('another size is not version 1.4', () => {
  assert.equal(page.checkCareer(new Uint8Array(1000)), 'not-14');
  assert.equal(page.checkCareer(save(() => {}, 0x4602c + 4)), 'not-14');
});
test('another signature or section size is not version 1.4', () => {
  assert.equal(page.checkCareer(save((v) => v.setUint32(0, 0x4d433033, true))), 'not-14');
  assert.equal(page.checkCareer(save((v) => v.setUint32(12, 0x45000, true))), 'not-14');
});
test('a checksummed file with another layout value or no game marker is refused', () => {
  assert.equal(page.checkCareer(save((v) => v.setUint32(0x24, 7, true))), 'not-14');
  assert.equal(page.checkCareer(save((v) => v.setUint32(0x2c + 0x50, 0, true))), 'not-14');
});
test('sizes that do not fit are refused', () => assert.equal(page.checkCareer(save((v) => v.setUint32(0x1c, 0x46000, true))), 'not-14'));
test('one changed byte anywhere is damage', () => {
  for (const at of [0x10, 0x1c, 0x2c, 0x1000, 0x4602b]) {
    const u8 = save(); u8[at] ^= 1;
    assert.equal(page.checkCareer(u8), 'corrupt', `byte ${at}`);
  }
});
test('careers are found by their folder, and strays are left out', () => {
  const found = page.findCareers([
    { path: 'Documents/NFS Carbon/RAZOR/RAZOR', data: save() },
    { path: 'NFS Carbon/RAZOR/notes.txt', data: new Uint8Array(4) },
    { path: 'NFS Carbon/ANA/ANA', data: new Uint8Array(4) },
    { path: 'NFS Carbon/../x', data: new Uint8Array(4) },
  ]);
  assert.equal(JSON.stringify(found.map((c) => [c.name, c.problem])), JSON.stringify([['RAZOR', null], ['ANA', 'not-14']]));
});
test('a file on its own takes its own name', () => assert.equal(page.findCareers([{ path: 'RAZOR', data: save() }])[0].name, 'RAZOR'));
