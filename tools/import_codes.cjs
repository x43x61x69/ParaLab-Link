// Sync ParaLab's Lab Sticker and Shop Codes without moving existing favorites.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

const [paralab] = process.argv.slice(2);
assert.ok(paralab, 'Usage: node tools/import_codes.cjs <ParaLab directory>');
const reference = require(path.resolve(paralab, 'src/collection_reference.json'));
const assets = path.resolve(__dirname, '../asset-pack');
const catalog = require(path.join(assets, 'catalog.json'));
const names = require(path.join(assets, 'item-names.json'));
assert.deepEqual(catalog.map(item => [item.kind, item.id]), names.items.map(row => [row.kind, row.id]));

const giftIds = new Set(catalog.filter(item => item.kind === 'gifts').map(item => String(item.id)));
for (const [id, code] of Object.entries(reference.shopCodes)) {
  assert.ok(giftIds.has(id) && /^\d{8}$/.test(code), `Invalid Shop Code entry for gift ${id}`);
}
for (const gift of catalog.filter(item => item.kind === 'gifts')) {
  gift.code = reference.shopCodes[gift.id] || '';
}

const existingCodes = new Set(catalog.filter(item => item.kind === 'codes').map(item => item.code));
const visitors = new Map(catalog.map((item, index) => [item, index])
  .filter(([item]) => item.kind === 'ghosts').map(([item, index]) => [item.id, index]));

for (const [id, entry] of Object.entries(reference.characters)) {
  assert.match(entry.code, /^[A-Z0-9]{8}$/);
  if (existingCodes.has(entry.code)) continue;
  const index = visitors.get(Number(id));
  assert.notEqual(index, undefined, `Missing visitor portrait for character ${id}`);
  const {image, atlas} = catalog[index];
  const labels = [...names.items[index].names];
  const originalName = entry.names[1];
  labels[0] = originalName;
  const nativeNames = Object.fromEntries(names.languages.slice(0, -1).map(([locale], i) => [locale, labels[i]]));
  catalog.push({
    kind: 'codes', id: Number(id), name: labels.at(-1), original_name: originalName,
    category: 5, field: 0, image, atlas, data: 0, blocks: 0, size: 0,
    uid: 0, char_uid: 0, code: entry.code, native_names: nativeNames,
  });
  names.items.push({kind: 'codes', id: Number(id), names: labels});
  existingCodes.add(entry.code);
}

fs.writeFileSync(path.join(assets, 'catalog.json'), JSON.stringify(catalog, null, 2) + '\n');
fs.writeFileSync(path.join(assets, 'item-names.json'), [
  '{', '  "languages": ' + JSON.stringify(names.languages) + ',', '  "items": [',
  names.items.map(row => '    ' + JSON.stringify(row)).join(',\n'), '  ]', '}',
].join('\n') + '\n');
