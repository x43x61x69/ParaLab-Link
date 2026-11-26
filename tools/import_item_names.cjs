// Regenerate the bundled names from a ParaLab checkout and local Paradise dumps.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

const [paralabPath, romPath] = process.argv.slice(2);
if (!paralabPath || !romPath) {
  throw new Error('Usage: node tools/import_item_names.cjs <ParaLab directory> <ROM dump directory>');
}
const source = path.resolve(paralabPath, 'src');
const core = require(path.join(source, 'core.js'));
const locale = require(path.join(source, 'rom_locale.js'));
const assets = path.join(__dirname, '../asset-pack');
const catalog = require(path.join(assets, 'catalog.json'));
const original = require(path.join(assets, 'catalog-source.json'));

const official = [
  ['en', 'English'], ['fr', 'Français'], ['es', 'Español'], ['de', 'Deutsch'],
  ['pt', 'Português'], ['it', 'Italiano'], ['ja', '日本語'], ['zh-Hans', '简体中文'], ['ko', '한국어'],
];
const languages = [...official, ['zh-Hant', '繁體中文']];
const normalize = name => name.normalize('NFKC').toUpperCase().replace(/[^A-Z0-9]/g, '');
const gifts = new Map(require(path.join(source, 'gift_catalog.json')).map(gift => [gift[0], gift]));
const lab = new Map(original.lab.map(item => [item.id, item.names]));
const nativeById = new Map();
const nativeByName = new Map();

for (const romFile of fs.readdirSync(romPath).sort().filter(name => /paradise.*\.bin$/i.test(name))) {
  const rom = fs.readFileSync(path.join(romPath, romFile));
  const {files} = core.openRom(rom);
  for (const row of core.charTable(files[0])) {
    const names = Object.fromEntries(official.map(([code]) => [code, locale.text(rom, row.name, code, {fallback: false})]));
    if (!names.en) continue;
    nativeById.set(row.id, names);
    nativeByName.set(normalize(names.en), names);
  }
}

// The Money Boost helper is a project item. Its ARC2 includes a reused Japanese label.
const money = Object.fromEntries(official.map(([code], i) => [code, [
  'Money Boost', 'Bonus de Gotchi Points', 'Bono de Gotchi Points', 'Gotchi-Punkte-Bonus',
  'Bônus de Gotchi Points', 'Bonus di punti Gotchi', 'ごっちポイントボーナス', '金币加成', '고치 포인트 보너스',
][i]]));

function localized(item) {
  if (item.native_names) return item.native_names;
  switch (item.kind) {
    case 'gifts': {
      const row = gifts.get(item.id);
      assert.ok(row, `Missing gift ${item.id}`);
      const names = [row[4], ...Object.values(row[6] || {}).map(variant => variant[0])];
      const matching = names.find(slots => normalize(locale.name(slots, 'en')) === normalize(item.original_name));
      assert.ok(matching, `No matching English gift name for ${item.id}`);
      return Object.fromEntries(official.map(([code]) => [code, locale.name(matching, code, {fallback: false})]));
    }
    case 'lab': {
      const names = lab.get(item.id);
      assert.ok(names, `Missing Lab Item ${item.id}`);
      return {...names, 'zh-Hans': names.zh};
    }
    case 'money':
      return money;
    case 'ghosts':
    case 'codes': {
      const name = item.original_name;
      const matches = nativeByName.get(normalize(name));
      const base = name.replace(/^(Forest|Glacier|Icy|Tropical) /, '');
      const native = nativeById.get(item.id);
      if (matches) return {...matches, en: name};
      assert.ok(item.kind === 'ghosts' && native && normalize(native.en) === normalize(base),
                `No matching character name for ${name} (${item.id})`);
      return {...native, en: name};
    }
    default:
      throw new Error(`Unknown catalog kind ${item.kind}`);
  }
}

const items = catalog.map(item => {
  const names = {...localized(item), 'zh-Hant': item.name};
  for (const [code] of languages) assert.ok(names[code], `Missing ${code} for ${item.kind} ${item.id}`);
  return {kind: item.kind, id: item.id, names: languages.map(([code]) => names[code])};
});
const output = ['{', '  "languages": ' + JSON.stringify(languages) + ',', '  "items": [',
  items.map(row => '    ' + JSON.stringify(row)).join(',\n'), '  ]', '}'];
fs.writeFileSync(path.join(assets, 'item-names.json'), output.join('\n') + '\n');
