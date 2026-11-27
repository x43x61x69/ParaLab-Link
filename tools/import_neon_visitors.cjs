// Append Neon Planets visitors and thumbnails to the bundled asset pack.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const zlib = require('node:zlib');
const {thumbnail, writer} = require('./asset_pack.cjs');

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

const firstNeon = catalog.findIndex(item => item.kind === 'ghosts' && item.field === neonWorld);
const isNeon = item => item.kind === 'ghosts' && item.field === neonWorld;
const lastNeon = firstNeon < 0 ? -1 : catalog.findLastIndex(isNeon);
const trailingItems = lastNeon < 0 ? [] : catalog.slice(lastNeon + 1);
let trailingRecords = [], trailingRecordStart = records.length;
let baseBytes = oldPack.length;
if (firstNeon >= 0) {
  const oldVisitors = catalog.slice(firstNeon, lastNeon + 1);
  assert.ok(oldVisitors.every(isNeon), 'Neon visitor range is not contiguous');
  const last = catalog[lastNeon];
  trailingRecordStart = last.data + last.blocks;
  trailingRecords = records.slice(trailingRecordStart);
  if (trailingItems.length) {
    // Saved favorites use the original catalog index, including entries added later.
    assert.deepEqual(visitors.map(({character}) => character.id), oldVisitors.map(item => item.id));
  }
  baseBytes = records[catalog[firstNeon].image][0];
  records.length = catalog[firstNeon].image;
  catalog.length = firstNeon;
}
const pack = writer(oldPack.subarray(0, baseBytes), records);
const {append} = pack;

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

let atlas = Math.max(...catalog.map(item => item.atlas)) + 1;
for (const visitor of visitors) {
  const {id, name: textRow} = visitor.character;
  const ghost = visit.buildRomGhost(visitor, core);
  const preview = portrait(ghost);
  visit.writeRomGhostPlanet(ghost, preview, core);
  assert.ok(visit.verifyGhost(ghost), `Invalid Neon ghost ${id}`);
  const image = append(thumbnail(preview, theme.dark.paper));
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
const recordOffset = records.length - trailingRecordStart;
for (const [offset, packed, raw, crc] of trailingRecords)
  pack.appendPacked(oldPack.subarray(offset, offset + packed), raw, crc);
for (const item of trailingItems) {
  if (item.image >= trailingRecordStart) item.image += recordOffset;
  if (item.blocks && item.data >= trailingRecordStart) item.data += recordOffset;
  catalog.push(item);
}
console.log('Packed ' + visitors.length + ' Neon Planets visitors, ' + pack.save(assets, catalog) + ' asset bytes');
