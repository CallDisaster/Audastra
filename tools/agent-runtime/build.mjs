import { build } from 'esbuild';
import { mkdir, writeFile, readFile, readdir, copyFile } from 'node:fs/promises';
import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { createHash } from 'node:crypto';

const root = dirname(fileURLToPath(import.meta.url));
const output = resolve(root, process.argv[2] || 'dist');
await mkdir(output, { recursive: true });
const result = await build({
  absWorkingDir: root, entryPoints: ['worker.mjs'],
  outfile: resolve(output, 'worker.bundle.mjs'), bundle: true,
  platform: 'node', target: 'node22', format: 'esm', splitting: false,
  metafile: true, legalComments: 'eof',
  banner: { js: 'import { createRequire as forgeCreateRequire } from "node:module"; const require = forgeCreateRequire(import.meta.url);' },
});
const packageRoots = new Set();
for (const source of Object.keys(result.metafile.inputs)) {
  const parts = source.replaceAll('\\', '/').split('/');
  const index = parts.lastIndexOf('node_modules');
  if (index < 0) continue;
  const end = index + (parts[index + 1].startsWith('@') ? 3 : 2);
  packageRoots.add(parts.slice(0, end).join('/'));
}
const packages = [];
for (const packageRoot of [...packageRoots].sort()) {
  const location = resolve(root, packageRoot);
  const pkg = JSON.parse(await readFile(resolve(location, 'package.json'), 'utf8'));
  const names = (await readdir(location)).filter(name => /^(LICENSE|LICENCE|COPYING|NOTICE)(\..*)?$/i.test(name));
  const fallback = resolve(root, 'licenses', `${pkg.name.replaceAll('/', '__')}.txt`);
  let sources = names.length ? names.map(name => resolve(location, name)) : [fallback];
  if (!names.length) {
    try {
      const readme = await readFile(resolve(location, 'README.md'), 'utf8');
      if (readme.includes('Permission is hereby granted, free of charge')) sources = [resolve(location, 'README.md')];
    } catch { /* A missing notice remains a hard failure below. */ }
  }
  const notices = [];
  for (const source of sources) {
    const bytes = await readFile(source);
    const name = `${pkg.name.replaceAll('/', '__')}__${source.split(/[\\/]/).at(-1)}`;
    await mkdir(resolve(output, 'licenses'), { recursive: true });
    await copyFile(source, resolve(output, 'licenses', name));
    notices.push({ file: `licenses/${name}`, sha256: createHash('sha256').update(bytes).digest('hex') });
  }
  packages.push({ name: pkg.name, version: pkg.version, license: pkg.license, notices });
}
const bundle = await readFile(resolve(output, 'worker.bundle.mjs'));
await writeFile(resolve(output, 'runtime-manifest.json'), JSON.stringify({
  runtime: 'pi', entry: 'worker.bundle.mjs', nodeMinimum: '22.19.0',
  sha256: createHash('sha256').update(bundle).digest('hex'), packages,
}, null, 2) + '\n');
console.log(`Portable runtime: ${bundle.length} bytes, ${packages.length} bundled packages; ${output}`);
