// 原版数据文本格式解析（SLK / profile INI / 字符串表），供 import-war3-data.mjs 使用。只读取，不包含任何游戏数据。
const decoder = new TextDecoder('utf-8');
export const text = bytes => decoder.decode(bytes).replace(/^﻿/, '');

/** SLK：返回 { columns, rows }；rows 以首列（ID）为键，值按列名存放，数字保留为数字。 */
export function parseSlk(source) {
  const cells = new Map();
  let x = 0, y = 0, maxX = 0, maxY = 0;
  for (const line of source.split(/\r?\n/)) {
    if (!line.startsWith('C;') && !line.startsWith('F;')) continue;
    let value;
    for (const part of line.split(';').slice(1)) {
      const key = part[0], rest = part.slice(1);
      if (key === 'X') x = Number(rest);
      else if (key === 'Y') y = Number(rest);
      else if (key === 'K' && line.startsWith('C;')) value = rest.startsWith('"') ? rest.slice(1, rest.endsWith('"') ? -1 : undefined) : Number.isFinite(Number(rest)) ? Number(rest) : rest;
    }
    if (value !== undefined) { cells.set(`${x},${y}`, value); maxX = Math.max(maxX, x); maxY = Math.max(maxY, y); }
  }
  const columns = [];
  for (let c = 1; c <= maxX; c++) columns[c] = cells.get(`${c},1`);
  const rows = new Map();
  for (let r = 2; r <= maxY; r++) {
    const id = cells.get(`1,${r}`);
    if (id === undefined || id === '') continue;
    const row = {};
    for (let c = 1; c <= maxX; c++) { const v = cells.get(`${c},${r}`); if (v !== undefined && columns[c] !== undefined) row[columns[c]] = v; }
    rows.set(String(id), row);
  }
  return { columns: columns.filter(Boolean), rows };
}

/** profile / INI：{ 节名: { 小写键: 原始值 } }；同名节后者覆盖同名键。 */
export function parseProfile(source, target = new Map()) {
  let section;
  for (const raw of source.split(/\r?\n/)) {
    const line = raw.trim();
    if (!line || line.startsWith('//') || line.startsWith(';')) continue;
    const header = line.match(/^\[(.+)\]$/);
    if (header) { section = header[1]; if (!target.has(section)) target.set(section, {}); continue; }
    const eq = line.indexOf('=');
    if (!section || eq < 0) continue;
    target.get(section)[line.slice(0, eq).trim().toLowerCase()] = line.slice(eq + 1);
  }
  return target;
}

/** profile 列表值：逗号分隔，引号内逗号不拆分，去掉包裹引号。 */
export function splitList(value) {
  if (value === undefined || value === null) return [];
  const items = [];
  let current = '', quoted = false;
  for (const ch of String(value)) {
    if (ch === '"') { quoted = !quoted; continue; }
    if (ch === ',' && !quoted) { items.push(current); current = ''; continue; }
    current += ch;
  }
  items.push(current);
  return items;
}

/** WorldEditStrings：键 → 文本，去掉菜单快捷键标记。 */
export function parseStrings(source, target = new Map()) {
  for (const raw of source.split(/\r?\n/)) {
    const eq = raw.indexOf('=');
    if (eq <= 0 || raw.startsWith('[') || raw.startsWith('//')) continue;
    const key = raw.slice(0, eq).trim();
    const value = raw.slice(eq + 1).replace(/\(&.\)/g, '').replace(/&/g, '').replace(/^"|"$/g, '');
    if (!target.has(key)) target.set(key, value);
  }
  return target;
}
