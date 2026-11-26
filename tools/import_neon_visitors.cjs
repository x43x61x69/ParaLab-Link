// Append Neon Planets visitors and thumbnails to the bundled asset pack.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const zlib = require('node:zlib');

const [paralabPath, romPath] = process.argv.slice(2);
if (!paralabPath || !romPath) {
  throw new Error('Usage: node tools/import_neon_visitors.cjs <ParaLab directory> <Neon Planets ROM>');
}
const source = path.resolve(paralabPath, 'src');
const core = require(path.join(source, 'core.js'));
const visit = require(path.join(source, 'visit.js'));
const locale = require(path.join(source, 'rom_locale.js'));
const assets = path.join(__dirname, '../asset-pack');
const catalog = require(path.join(assets, 'catalog.json'));
const records = require(path.join(assets, 'records.json'));
const theme = require(path.join(assets, 'ui-theme.json'));
const oldPack = fs.readFileSync(path.join(assets, 'assets.bin'));
assert.equal(oldPack.subarray(0, 4).toString(), 'KPA1');
assert.equal(oldPack.readUInt32LE(4), oldPack.length);
assert.equal(oldPack.readUInt32LE(8), records.length);

const rom = fs.readFileSync(romPath);
const context = core.openRom(rom);
assert.equal(context.editionId, 8, 'Expected a Neon Planets ROM');
const neonWorld = visit.ED.neon;
const visitors = visit.collectRomVisitors(context, core, {nativeAnimationFallback: true})
  .filter(({character}) => character.field >= 7 && character.field <= 9)
  .sort((a, b) => a.character.field - b.character.field || a.character.id - b.character.id);
assert.ok(visitors.length && new Set(visitors.map(v => v.character.id)).size === visitors.length);

const oldNeon = catalog.findIndex(item => item.kind === 'ghosts' && item.field === neonWorld);
const baseBytes = oldNeon < 0 ? oldPack.length : records[catalog[oldNeon].image][0];
if (oldNeon >= 0) {
  assert.ok(catalog.slice(oldNeon).every(item => item.kind === 'ghosts' && item.field === neonWorld));
  records.length = catalog[oldNeon].image;
  catalog.length = oldNeon;
}
const chunks = [Buffer.from(oldPack.subarray(0, baseBytes))];
let packSize = baseBytes;

function append(bytes) {
  const packed = zlib.deflateSync(bytes, {level: 9});
  const index = records.length;
  records.push([packSize, packed.length, bytes.length, zlib.crc32(bytes)]);
  chunks.push(packed);
  packSize += packed.length;
  return index;
}

function portrait(ghost) {
  const view = new DataView(ghost.buffer, ghost.byteOffset, ghost.byteLength);
  // Same neutral pose, positioning and transparent compositing as ParaLab's visitor preview.
  const layers = Array.from({length: 3}, (_, part) => {
    const spriteStart = view.getUint32(0x110 + 8 * part, true);
    const spriteEnd = spriteStart + view.getUint32(0x114 + 8 * part, true);
    const pose = 0x600 + 110 + 22 * part;
    assert.equal(view.getUint16(pose, true), part);
    const sprite = core.spriteDecode(ghost.subarray(spriteStart, spriteEnd), 0, true)
      .subs[view.getUint16(pose + 12, true)];
    assert.ok(sprite);
    return {
      sprite,
      x: view.getInt16(pose + 2, true) + view.getInt8(spriteStart + 10),
      y: view.getInt16(pose + 4, true) + view.getInt8(spriteStart + 11),
    };
  });
  for (const layer of layers) layer.y += Math.trunc((layers[0].sprite.h - layer.sprite.h) / 2);
  const left = Math.min(...layers.map(l => l.x));
  const top = Math.min(...layers.map(l => l.y));
  const width = Math.max(...layers.map(l => l.x + l.sprite.w)) - left;
  const height = Math.max(...layers.map(l => l.y + l.sprite.h)) - top;
  const image = core.emptyImage(width, height);
  for (const {sprite, x, y} of layers) {
    for (let row = 0; row < sprite.h; ++row) for (let col = 0; col < sprite.w; ++col) {
      const offset = (row * sprite.w + col) * 4;
      if (sprite.data[offset + 3]) {
        image.data.set(sprite.data.subarray(offset, offset + 4), ((y - top + row) * width + x - left + col) * 4);
      }
    }
  }
  return image;
}

function thumbnail(image) {
  const rgb565 = (r, g, b) => (r >> 3) << 11 | (g >> 2) << 5 | (b >> 3);
  const paper = rgb565(...Buffer.from(theme.dark.paper, 'hex'));
  const scale = Math.min(1, 32 / Math.max(image.w, image.h));
  const width = Math.max(1, Math.round(image.w * scale)), height = Math.max(1, Math.round(image.h * scale));
  const x = (32 - width) >> 1, y = (32 - height) >> 1;
  const tile = Buffer.alloc(2048);
  for (let offset = 0; offset < tile.length; offset += 2) tile.writeUInt16LE(paper, offset);
  for (let row = 0; row < height; ++row) for (let col = 0; col < width; ++col) {
    const offset = (Math.floor(row * image.h / height) * image.w + Math.floor(col * image.w / width)) * 4;
    if (image.data[offset + 3]) {
      tile.writeUInt16LE(rgb565(...image.data.subarray(offset, offset + 3)), 2 * ((y + row) * 32 + x + col));
    }
  }
  assert.ok(tile.some((byte, offset) => byte !== tile[offset % 2]), 'Blank Neon visitor thumbnail');
  return tile;
}

let atlas = Math.max(...catalog.map(item => item.atlas)) + 1;
for (const visitor of visitors) {
  const {id, name: textRow} = visitor.character;
  const ghost = visit.buildRomGhost(visitor, core);
  const preview = portrait(ghost);
  visit.writeRomGhostPlanet(ghost, preview, core);
  assert.ok(visit.verifyGhost(ghost), `Invalid Neon ghost ${id}`);
  const image = append(thumbnail(preview));
  const data = records.length;
  for (let offset = 0; offset < ghost.length; offset += 4096) append(ghost.subarray(offset, offset + 4096));
  const names = Object.fromEntries(locale.LOCALES.map(([code]) => [code, locale.text(rom, textRow, code, {fallback: false})]));
  assert.ok(Object.values(names).every(Boolean), `Incomplete native names for ${id}`);
  const fields = visit.playdateFields(null, ghost, neonWorld);
  catalog.push({
    kind: 'ghosts', id, name: locale.taiwanText(names['zh-Hans']), original_name: names.en,
    category: 4, field: neonWorld, image, atlas: atlas++, data, blocks: records.length - data,
    size: ghost.length, uid: fields.visDeviceUid, char_uid: fields.visCharacterUid, code: '', native_names: names,
  });
}
assert.ok(packSize <= 0x480000, 'Asset pack exceeds its flash partition');
const pack = Buffer.concat(chunks, packSize);
pack.writeUInt32LE(pack.length, 4);
pack.writeUInt32LE(records.length, 8);
pack.writeUInt32LE(zlib.crc32(pack.subarray(16)), 12);
fs.writeFileSync(path.join(assets, 'assets.bin'), pack);
fs.writeFileSync(path.join(assets, 'records.json'), JSON.stringify(records) + '\n');
fs.writeFileSync(path.join(assets, 'catalog.json'), JSON.stringify(catalog, null, 2) + '\n');
console.log(`Packed ${visitors.length} Neon Planets visitors, ${packSize} asset bytes`);
