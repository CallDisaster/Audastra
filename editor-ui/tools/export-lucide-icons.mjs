// 从 lucide-react 的图标数据导出原生端使用的 SVG（ISC 授权，随文件附授权文本）。
// 线宽统一为 1.75，颜色为 currentColor，由原生 SVG 图标引擎按主题着色。
// 用法（在 editor-ui 目录）：node tools/export-lucide-icons.mjs
import { copyFileSync, mkdirSync, readFileSync, readdirSync, rmSync, writeFileSync } from 'node:fs';
import { createRequire } from 'node:module';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const require = createRequire(import.meta.url);
const packageRoot = dirname(require.resolve('lucide-react/package.json'));
const version = JSON.parse(readFileSync(join(packageRoot, 'package.json'), 'utf8')).version;
const outDir = resolve(here, '../../assets/icons/lucide');

// 原生端实际用到的通用图标；新增时在这里登记后重新导出。
const icons = [
  'app-window', 'arrow-left', 'arrow-right', 'audio-lines', 'box', 'check', 'chevron-down',
  'chevron-right', 'chevron-up', 'circle-dot', 'code-xml', 'columns-3', 'command', 'copy',
  'database', 'eye', 'eye-off', 'file', 'file-audio', 'file-code-2', 'file-plus-2', 'flag',
  'folder', 'folder-open', 'folder-plus', 'git-branch', 'grip-vertical', 'hand', 'hash',
  'image', 'layers', 'layout-dashboard', 'layout-grid', 'list-filter', 'list-tree', 'map',
  'map-pin', 'maximize-2', 'minimize-2', 'minus', 'mountain', 'mouse-pointer-2', 'music-2',
  'panel-right', 'panel-top', 'pencil', 'pin', 'play', 'plus', 'power', 'puzzle', 'redo-2',
  'rotate-ccw', 'save', 'search', 'settings-2', 'shield', 'sliders-horizontal', 'square',
  'square-dashed', 'target', 'trash-2', 'trees', 'triangle-alert', 'undo-2', 'users',
  'variable', 'volume-2', 'x', 'zap',
];

const escape = (value) => String(value).replace(/&/g, '&amp;').replace(/"/g, '&quot;').replace(/</g, '&lt;');

function renderNode([tag, attrs]) {
  const parts = Object.entries(attrs)
    .filter(([name]) => name !== 'key')
    .map(([name, value]) => `${name}="${escape(value)}"`);
  return `  <${tag} ${parts.join(' ')}/>`;
}

rmSync(outDir, { recursive: true, force: true });
mkdirSync(outDir, { recursive: true });
const available = new Set(readdirSync(join(packageRoot, 'dist/esm/icons')).map((file) => file.replace(/\.js$/, '')));
const missing = icons.filter((name) => !available.has(name));
if (missing.length) throw new Error(`lucide-react ${version} has no icon(s): ${missing.join(', ')}`);

// 旧名（如 file-code-2）只是 `export { default } from './新名.js'`，沿别名找到真正的图标数据。
async function iconNode(name, seen = new Set()) {
  if (seen.has(name)) throw new Error(`alias cycle at ${name}`);
  seen.add(name);
  const file = join(packageRoot, 'dist/esm/icons', `${name}.js`);
  const module = await import(pathToFileURL(file).href);
  if (module.__iconNode) return module.__iconNode;
  const alias = /export \{ default \} from '\.\/([a-z0-9-]+)\.js'/.exec(readFileSync(file, 'utf8'));
  if (!alias) throw new Error(`no icon data in ${name}`);
  return iconNode(alias[1], seen);
}

for (const name of icons) {
  const body = (await iconNode(name)).map(renderNode).join('\n');
  const svg = [
    `<!-- Lucide ${name} (lucide-react ${version}, ISC) -->`,
    '<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.75" stroke-linecap="round" stroke-linejoin="round">',
    body,
    '</svg>',
    '',
  ].join('\n');
  writeFileSync(join(outDir, `${name}.svg`), svg);
}
copyFileSync(join(packageRoot, 'LICENSE'), join(outDir, 'LICENSE'));
console.log(`exported ${icons.length} icons from lucide-react ${version} to ${outDir}`);
