const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const zlib = require('node:zlib');

function thumbnail(image, background) {
  const rgb565 = (r, g, b) => (r >> 3) << 11 | (g >> 2) << 5 | (b >> 3);
  const paper = rgb565(...Buffer.from(background, 'hex'));
  const scale = Math.min(1, 32 / Math.max(image.w, image.h));
  const width = Math.max(1, Math.round(image.w * scale));
  const height = Math.max(1, Math.round(image.h * scale));
  const x = (32 - width) >> 1, y = (32 - height) >> 1;
  const tile = Buffer.alloc(2048);
  for (let offset = 0; offset < tile.length; offset += 2) tile.writeUInt16LE(paper, offset);
  for (let row = 0; row < height; ++row) for (let col = 0; col < width; ++col) {
    const source = (Math.floor(row * image.h / height) * image.w + Math.floor(col * image.w / width)) * 4;
    if (image.data[source + 3]) {
      tile.writeUInt16LE(rgb565(...image.data.subarray(source, source + 3)), 2 * ((y + row) * 32 + x + col));
    }
  }
  assert.ok(tile.some((byte, offset) => byte !== tile[offset % 2]), 'Blank thumbnail');
  return tile;
}

function writer(pack, records) {
  const chunks = [Buffer.from(pack)];
  let size = pack.length;
  function appendPacked(packed, raw, crc) {
    const index = records.length;
    records.push([size, packed.length, raw, crc]);
    chunks.push(packed);
    size += packed.length;
    return index;
  }
  function append(bytes) {
    return appendPacked(zlib.deflateSync(bytes, {level: 9}), bytes.length, zlib.crc32(bytes));
  }
  function save(directory, catalog) {
    assert.ok(size <= 0x480000, 'Asset pack exceeds its flash partition');
    const output = Buffer.concat(chunks, size);
    output.writeUInt32LE(size, 4);
    output.writeUInt32LE(records.length, 8);
    output.writeUInt32LE(zlib.crc32(output.subarray(16)), 12);
    fs.writeFileSync(path.join(directory, 'assets.bin'), output);
    fs.writeFileSync(path.join(directory, 'records.json'), JSON.stringify(records) + '\n');
    fs.writeFileSync(path.join(directory, 'catalog.json'), JSON.stringify(catalog, null, 2) + '\n');
    return size;
  }
  return {append, appendPacked, save};
}

module.exports = {thumbnail, writer};
