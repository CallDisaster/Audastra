// 从本机经典版 Warcraft III 安装导出物编原型所需的只读数据（字段元数据、枚举、标准物体、按钮图标）。
// 用法：node tools/import-war3-data.mjs --game "<魔兽争霸 III 安装目录>" [--cli <war3-map-cli.exe>] [--no-icons]
// 输出到 public/local-data/war3/（已被 .gitignore 忽略）。导出内容是暴雪数据，只供本机原型使用，禁止提交或分发。
import { execFile } from 'node:child_process';
import { existsSync, mkdirSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { promisify } from 'node:util';
import { deflateSync } from 'node:zlib';
import { parseProfile, parseSlk, parseStrings, splitList, text } from './war3-parse.mjs';

const run = promisify(execFile);
const here = dirname(fileURLToPath(import.meta.url));
const root = resolve(here, '..');
const args = process.argv.slice(2);
const option = (name, fallback) => { const i = args.indexOf(name); return i >= 0 ? args[i + 1] : fallback; };
const game = option('--game', process.env.WAR3_ROOT);
const cli = resolve(option('--cli', join(root, '../build/vs2026/src/Cli/Release/war3-map-cli.exe')));
const withIcons = !args.includes('--no-icons');
if (!game) { console.error('需要 --game <Warcraft III 安装目录> 或 WAR3_ROOT'); process.exit(2); }
if (!existsSync(cli)) { console.error(`找不到 war3-map-cli：${cli}`); process.exit(2); }
const cache = join(root, 'local-data/raw');
const out = join(root, 'public/local-data/war3');
mkdirSync(cache, { recursive: true }); mkdirSync(join(out, 'icons'), { recursive: true });

const loaded = [], missing = [];
async function extract(internal) {
  const target = join(cache, internal.replaceAll('\\', '__').replaceAll('/', '__'));
  if (!existsSync(target)) {
    try { await run(cli, ['game-extract', game, internal.replaceAll('/', '\\'), target]); }
    catch { missing.push(internal); return null; }
  }
  loaded.push(internal);
  return readFileSync(target);
}
const slk = async path => { const bytes = await extract(path); return bytes ? parseSlk(text(bytes)) : { columns: [], rows: new Map() }; };
async function profiles(paths) { const merged = new Map(); for (const path of paths) { const bytes = await extract(path); if (bytes) parseProfile(text(bytes), merged); } return merged; }

// ---- 字符串 ----
const strings = new Map();
for (const path of ['UI/WorldEditStrings.txt', 'UI/WorldEditGameStrings.txt']) { const bytes = await extract(path); if (bytes) parseStrings(text(bytes), strings); }
const str = (value, depth = 0) => { const key = String(value ?? ''); if (!/^WESTRING_/.test(key) || depth > 6) return key; const next = strings.get(key); return next === undefined ? key : str(next, depth + 1); };
const editorData = parseProfile(text(await extract('UI/UnitEditorData.txt') ?? new Uint8Array()));
const worldData = parseProfile(text(await extract('UI/WorldEditData.txt') ?? new Uint8Array()));

// 字段类别名。本机汉化把 art 译成“技能”，与原版编辑器实际显示的“显示”不符，这里按 YDWE 所见修正。
const categories = {};
for (const [key, value] of Object.entries(worldData.get('ObjectEditorCategories') ?? {})) categories[key] = str(value);
categories.art = '显示';

// 枚举：UnitEditorData 中“序号=值,WESTRING”；标志列表类型（targetList 等）同样使用。
const enums = {};
for (const [name, entries] of editorData) {
  const values = Object.entries(entries).filter(([key]) => /^\d+$/.test(key)).sort(([a], [b]) => Number(a) - Number(b)).map(([, raw]) => { const [value, label] = splitList(raw); return { value, label: str(label) || value }; }).filter(item => item.value !== undefined);
  if (values.length) enums[name] = values;
}
const categoryLabels = section => Object.fromEntries(Object.entries(worldData.get(section) ?? {}).map(([key, raw]) => [key.toUpperCase(), str(splitList(raw)[0])]));
const destructableCategories = categoryLabels('DestructibleCategories'), doodadCategories = categoryLabels('DoodadCategories');
// 地形设置与装饰物/可破坏物类别在 WorldEditData 中定义，补成同样的枚举形式。
enums.tilesetList = [{ value: '*', label: '全部' }, ...Object.entries(worldData.get('TileSets') ?? {}).map(([value, raw]) => ({ value: value.toUpperCase(), label: str(splitList(raw)[0]) }))];
enums.destructableCategory = Object.entries(destructableCategories).map(([value, label]) => ({ value, label }));
enums.doodadCategory = Object.entries(doodadCategories).map(([value, label]) => ({ value, label }));
// 原版物编目录的第一层分组（与 World Editor 计数一致：人族 108、中立敌对 292…；技能 人族 52、特殊 289…）。
// 四个主种族按种族；单位的其他种族按 UnitUI.hostilePal 分为中立敌对 / 中立被动，娜迦单独成组；
// 技能中 creeps 为中立敌对、naga 为中立被动、其余（含物品技能）为特殊。
const mainRaces = { human: '人族', orc: '兽族', undead: '不死族', nightelf: '暗夜精灵族' };
const unitRaceGroup = (race, hostile) => mainRaces[race] ?? (race === 'naga' ? '中立 - 娜迦' : hostile ? '中立敌对' : '中立被动');
const abilityRaceGroup = race => mainRaces[race] ?? (race === 'creeps' ? '中立敌对' : race === 'naga' ? '中立被动' : '特殊');
const enumLabel = (name, value) => enums[name]?.find(item => item.value.toLowerCase() === String(value).toLowerCase())?.label ?? String(value);

// ---- profile 文件 ----
const races = ['Campaign', 'Human', 'Neutral', 'NightElf', 'Orc', 'Undead'];
const unitProfiles = await profiles(races.flatMap(race => [`Units/${race}UnitFunc.txt`, `Units/${race}UnitStrings.txt`]));
const abilityProfiles = await profiles([...races, 'Common', 'Item'].flatMap(race => [`Units/${race}AbilityFunc.txt`, `Units/${race}AbilityStrings.txt`]));
const upgradeProfiles = await profiles(races.flatMap(race => [`Units/${race}UpgradeFunc.txt`, `Units/${race}UpgradeStrings.txt`]));
const itemProfiles = await profiles(['Units/ItemFunc.txt', 'Units/ItemStrings.txt']);

const empty = value => value === undefined || value === null || value === '-' || value === '_' || value === '';
// 字符串值中的 WESTRING 键（可破坏物/装饰物名称等）按编辑器显示展开。
const clean = value => empty(value) ? '' : typeof value === 'string' && value.startsWith('WESTRING_') ? str(value) : value;
/** 行内列名大小写不一致时的查找。 */
const column = (row, name) => { if (!row) return undefined; if (name in row) return row[name]; const lower = name.toLowerCase(); for (const key in row) if (key.toLowerCase() === lower) return row[key]; return undefined; };

const specs = {
  unit: { meta: 'Units/UnitMetaData.slk', tables: { UnitData: 'Units/UnitData.slk', UnitUI: 'Units/UnitUI.slk', UnitBalance: 'Units/UnitBalance.slk', UnitWeapons: 'Units/UnitWeapons.slk', UnitAbilities: 'Units/UnitAbilities.slk' }, primary: 'UnitData', profiles: unitProfiles, applies: m => m.useUnit || m.useHero || m.useBuilding },
  item: { meta: 'Units/UnitMetaData.slk', tables: { ItemData: 'Units/ItemData.slk' }, primary: 'ItemData', profiles: itemProfiles, applies: m => m.useItem },
  ability: { meta: 'Units/AbilityMetaData.slk', tables: { AbilityData: 'Units/AbilityData.slk' }, primary: 'AbilityData', profiles: abilityProfiles, applies: () => true },
  buff: { meta: 'Units/AbilityBuffMetaData.slk', tables: { AbilityBuffData: 'Units/AbilityBuffData.slk' }, primary: 'AbilityBuffData', profiles: abilityProfiles, applies: () => true },
  upgrade: { meta: 'Units/UpgradeMetaData.slk', tables: { UpgradeData: 'Units/UpgradeData.slk' }, primary: 'UpgradeData', profiles: upgradeProfiles, applies: () => true },
  destructable: { meta: 'Units/DestructableMetaData.slk', tables: { DestructableData: 'Units/DestructableData.slk' }, primary: 'DestructableData', profiles: new Map(), applies: () => true },
  doodad: { meta: 'Doodads/DoodadMetaData.slk', tables: { Doodads: 'Doodads/Doodads.slk' }, primary: 'Doodads', profiles: new Map(), applies: () => true },
};

const index = { generatedAt: new Date().toISOString(), source: 'Warcraft III 经典版本机安装（只读导出）', categories, enums, kinds: {} };
const iconPaths = new Map();
const iconFile = path => { if (!path) return ''; let key = path.replaceAll('/', '\\').trim(); if (!/\.blp$/i.test(key)) key = key.replace(/\.[a-z]+$/i, '') + '.blp'; const name = key.toLowerCase().replace(/[^a-z0-9]+/g, '_').replace(/_blp$/, '') + '.png'; iconPaths.set(name, key); return name; };

for (const [kind, spec] of Object.entries(specs)) {
  const meta = await slk(spec.meta);
  const tables = {};
  for (const [name, path] of Object.entries(spec.tables)) tables[name] = (await slk(path)).rows;
  const primary = tables[spec.primary];
  const fields = [...meta.rows.values()].filter(m => spec.applies(m)).map(m => ({
    id: m.ID, field: m.field, slk: String(m.slk), index: Number(m.index ?? -1), repeat: Number(m.repeat ?? 0), data: Number(m.data ?? 0),
    category: m.category, name: str(m.displayName).replace('%s', m.effectType === 'Mod' ? '等级增量' : m.effectType === 'Code' ? '代码' : '基础值'), sort: String(m.sort ?? ''), type: String(m.type), min: m.minVal, max: m.maxVal,
    canBeEmpty: !!m.canBeEmpty, useUnit: !!m.useUnit, useHero: !!m.useHero, useBuilding: !!m.useBuilding, useItem: !!m.useItem, useCreep: !!m.useCreep,
    specific: m.useSpecific ? String(m.useSpecific).split(',').filter(Boolean) : undefined, notSpecific: m.notSpecific ? String(m.notSpecific).split(',').filter(Boolean) : undefined,
  })).sort((a, b) => a.sort.localeCompare(b.sort));
  const objects = [], values = {};
  for (const [id, row] of primary) {
    const profile = spec.profiles.get(id) ?? {};
    const code = String(column(row, 'code') ?? id);
    const levels = kind === 'ability' ? Math.max(1, Number(column(row, 'levels')) || 1) : kind === 'upgrade' ? Math.max(1, Number(column(row, 'maxlevel')) || 1) : 1;
    let group = [], applies = () => true, name = '', suffix = clean(profile.editorsuffix ?? column(row, 'EditorSuffix')), icon = '', hidden = false, race = '';
    if (kind === 'unit') {
      const ui = tables.UnitUI.get(id), balance = tables.UnitBalance.get(id);
      race = String(column(row, 'race') ?? 'other');
      const hero = /^[A-Z]/.test(id), building = String(column(row, 'isbldg') ?? column(balance, 'isbldg') ?? column(ui, 'isbldg') ?? '0') === '1';
      const special = Number(column(ui, 'special')) === 1, campaign = Number(column(ui, 'campaign')) === 1;
      hidden = Number(column(ui, 'inEditor')) === 0 || Number(column(ui, 'hiddenInEditor')) === 1;
      group = [unitRaceGroup(race, Number(column(ui, 'hostilePal')) === 1), campaign ? '战役' : '对战', special ? '特殊' : hero ? '英雄' : building ? '建筑' : '单位'];
      applies = f => hero ? f.useHero : building ? f.useBuilding : f.useUnit;
      name = profile.name ?? column(ui, 'name') ?? id; icon = splitList(profile.art)[0];
    } else if (kind === 'item') {
      group = [enumLabel('itemClass', column(row, 'class'))]; name = profile.name ?? id; icon = splitList(profile.art)[0];
    } else if (kind === 'ability') {
      race = String(column(row, 'race') ?? 'other');
      const hero = Number(column(row, 'hero')) === 1, item = Number(column(row, 'item')) === 1;
      hidden = Number(column(row, 'useInEditor')) === 0;
      group = [abilityRaceGroup(item ? 'other' : race), item ? '物品' : hero ? '英雄' : '单位'];
      applies = f => (!f.specific || f.specific.includes(code)) && !(f.notSpecific ?? []).includes(code) && (item ? f.useItem : hero ? f.useHero : race === 'creeps' ? f.useCreep || f.useUnit : f.useUnit);
      name = profile.name ?? id; icon = splitList(profile.art)[0];
    } else if (kind === 'buff') {
      race = String(column(row, 'race') ?? 'other');
      hidden = Number(column(row, 'useInEditor')) === 0;
      group = [abilityRaceGroup(race), Number(column(row, 'isEffect')) === 1 ? '效果' : '魔法效果'];
      name = profile.editorname ?? profile.bufftip ?? id; icon = splitList(profile.buffart)[0];
    } else if (kind === 'upgrade') {
      race = String(column(row, 'race') ?? 'other');
      group = [mainRaces[race] ?? (race === 'naga' ? '中立 - 娜迦' : '特殊')]; name = splitList(profile.name)[0] ?? id; icon = splitList(profile.art)[0];
    } else if (kind === 'destructable') {
      group = [destructableCategories[String(column(row, 'category')).toUpperCase()] ?? String(column(row, 'category'))]; name = str(column(row, 'Name')) || id;
    } else {
      group = [doodadCategories[String(column(row, 'category')).toUpperCase()] ?? String(column(row, 'category'))]; name = str(column(row, 'Name')) || id;
    }
    suffix = str(suffix);
    const own = {};
    for (const field of fields) {
      if (!applies(field)) continue;
      let value;
      if (field.slk === 'Profile') {
        const raw = profile[field.field.toLowerCase()];
        if (field.repeat > 0) { const list = splitList(raw); value = Array.from({ length: levels }, (_, i) => clean(list[Math.min(i, list.length - 1)] ?? '')); }
        else value = field.index >= 0 && !/List$/i.test(field.type) ? clean(splitList(raw)[field.index] ?? '') : clean(raw ?? '');
      } else {
        const table = tables[field.slk] ?? primary, source = table.get(id);
        if (field.repeat > 0 && kind === 'ability') value = Array.from({ length: levels }, (_, i) => clean(column(source, `${field.field}${field.data > 0 ? 'ABCDEFGHI'[field.data - 1] : ''}${i + 1}`)));
        else { const raw = column(source, field.field); value = field.index >= 0 && typeof raw === 'string' && raw.includes(',') && !/List$/i.test(field.type) ? clean(raw.split(',')[field.index]) : clean(raw); }
      }
      own[field.id] = value;
    }
    values[id] = own;
    objects.push({ id, name: String(name), suffix: suffix === '_' ? '' : suffix, group, race, hidden, levels, code, icon: iconFile(icon) });
  }
  const used = new Set(Object.values(values).flatMap(Object.keys));
  index.kinds[kind] = { fields: fields.filter(field => used.has(field.id)).map(({ useUnit, useHero, useBuilding, useItem, useCreep, ...rest }) => rest), objects };
  writeFileSync(join(out, `values-${kind}.json`), JSON.stringify(values));
  console.log(`${kind}: ${objects.length} 个物体，${index.kinds[kind].fields.length} 个字段`);
}

// ---- 图标：BLP → PPM（war3-map-cli）→ PNG ----
function png(width, height, rgb) {
  const crcTable = Array.from({ length: 256 }, (_, n) => { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; return c >>> 0; });
  const crc = bytes => { let c = 0xffffffff; for (const b of bytes) c = crcTable[(c ^ b) & 0xff] ^ (c >>> 8); return (c ^ 0xffffffff) >>> 0; };
  const chunk = (type, data) => { const body = Buffer.concat([Buffer.from(type), data]); const length = Buffer.alloc(4); length.writeUInt32BE(data.length); const sum = Buffer.alloc(4); sum.writeUInt32BE(crc(body)); return Buffer.concat([length, body, sum]); };
  const header = Buffer.alloc(13); header.writeUInt32BE(width, 0); header.writeUInt32BE(height, 4); header[8] = 8; header[9] = 2;
  const raw = Buffer.alloc((width * 3 + 1) * height);
  for (let y = 0; y < height; y++) rgb.copy(raw, y * (width * 3 + 1) + 1, y * width * 3, (y + 1) * width * 3);
  return Buffer.concat([Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]), chunk('IHDR', header), chunk('IDAT', deflateSync(raw)), chunk('IEND', Buffer.alloc(0))]);
}
function readPpm(bytes) {
  let offset = 0; const tokens = [];
  while (tokens.length < 4) { while (/\s/.test(String.fromCharCode(bytes[offset]))) offset++; let token = ''; while (!/\s/.test(String.fromCharCode(bytes[offset]))) token += String.fromCharCode(bytes[offset++]); tokens.push(token); }
  offset++;
  if (tokens[0] !== 'P6') throw new Error('不是 P6 PPM');
  return { width: Number(tokens[1]), height: Number(tokens[2]), rgb: bytes.subarray(offset) };
}
const icons = { converted: 0, failed: [] };
if (withIcons) {
  const queue = [...iconPaths.entries()].filter(([name]) => !existsSync(join(out, 'icons', name)));
  const work = async () => {
    for (let item = queue.shift(); item; item = queue.shift()) {
      const [name, internal] = item;
      const blp = join(cache, 'icon-' + name.replace(/\.png$/, '.blp')), ppm = blp.replace(/\.blp$/, '.ppm');
      try {
        if (!existsSync(blp)) await run(cli, ['game-extract', game, internal, blp]);
        await run(cli, ['blp-decode', blp, ppm]);
        const image = readPpm(readFileSync(ppm));
        writeFileSync(join(out, 'icons', name), png(image.width, image.height, Buffer.from(image.rgb)));
        rmSync(ppm, { force: true }); icons.converted++;
      } catch { icons.failed.push(internal); }
    }
  };
  await Promise.all(Array.from({ length: 8 }, work));
}
index.icons = { available: withIcons, failed: icons.failed.length };
index.files = [...new Set(loaded)].sort();
index.missing = [...new Set(missing)].sort();
writeFileSync(join(out, 'index.json'), JSON.stringify(index));
console.log(`图标：新转换 ${icons.converted}，失败 ${icons.failed.length}${icons.failed.length ? `（如 ${icons.failed.slice(0, 3).join('，')}）` : ''}`);
console.log(`读取 ${index.files.length} 个文件，缺失 ${index.missing.length} 个${index.missing.length ? `：${index.missing.join('，')}` : ''}`);
