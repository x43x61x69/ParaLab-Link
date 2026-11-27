// Append Lab Items by ID without changing existing catalog positions (used by saved favorites).
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const zlib = require('node:zlib');
const {thumbnail, writer} = require('./asset_pack.cjs');

const [paraLabPath] = process.argv.slice(2);
if (!paraLabPath) throw new Error('Usage: node tools/import_lab_items.cjs <ParaLab directory>');
const source = path.resolve(paraLabPath);
const core = require(path.join(source, 'src/core.js'));
const labItems = require(path.join(source, 'src/lab_items.js'));
const locale = require(path.join(source, 'src/rom_locale.js'));
const bundle = path.join(source, 'assets/lab-items');
const assets = path.join(__dirname, '../asset-pack');
const catalog = require(path.join(assets, 'catalog.json'));
const records = require(path.join(assets, 'records.json'));
const theme = require(path.join(assets, 'ui-theme.json'));
const pack = fs.readFileSync(path.join(assets, 'assets.bin'));
assert.equal(pack.subarray(0, 4).toString(), 'KPA1');
assert.equal(pack.readUInt32LE(4), pack.length);
assert.equal(pack.readUInt32LE(8), records.length);
assert.equal(pack.readUInt32LE(12), zlib.crc32(pack.subarray(16)));

const {append, save} = writer(pack, records);
const known = new Set(catalog.filter(item => item.kind === 'lab').map(item => item.id));
let atlas = Math.max(...catalog.map(item => item.atlas)) + 1;
let added = 0;
for (const {filename} of require(path.join(bundle, 'catalog.json'))) {
  const item = labItems.forTransfer(fs.readFileSync(path.join(bundle, filename)), core);
  assert.ok(item, 'Invalid Lab Item: ' + filename);
  if (known.has(item.id)) continue;
  const preview = labItems.thumbnail(item, core);
  assert.ok(preview, 'Missing preview: ' + filename);
  // Match the existing ParaLink Lab Items' buy and sell prices.
  const priced = labItems.withPrices(item, 0, 9999, core);
  const checked = labItems.forTransfer(priced.bytes, core);
  assert.ok(checked && checked.id === item.id && checked.buy === 0 && checked.sell === 9999, filename);
  const native_names = Object.fromEntries(locale.LOCALES.map(([code], i) => [code, checked.names[i]]));
  assert.ok(Object.values(native_names).every(Boolean), 'Incomplete item names: ' + filename);
  const image = append(thumbnail(preview, theme.dark.paper));
  const data = records.length;
  for (let offset = 0; offset < priced.bytes.length; offset += 4096)
    append(priced.bytes.subarray(offset, offset + 4096));
  catalog.push({
    kind: 'lab', id: item.id, name: locale.taiwanText(native_names['zh-Hans']), original_name: native_names.en,
    category: 3, field: 0, image, atlas: atlas++, data, blocks: records.length - data,
    size: priced.bytes.length, uid: 0, char_uid: 0, code: '', native_names,
  });
  console.log('Added 0x' + item.id.toString(16).toUpperCase() + ' ' + native_names.en);
  known.add(item.id);
  ++added;
}
if (added) console.log('Packed ' + added + ' Lab Items into ' + save(assets, catalog) + ' bytes');
else console.log('Lab Item IDs are up to date.');
