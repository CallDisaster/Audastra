// CPU 单工作线程导出；保留已批准 SVG，只调整应用图标的画布和圆角外透明区域。
// node export-brand.cjs <master.svg> <output-directory> <sharp-module-path>
const fs = require('fs');
const path = require('path');
const crypto = require('crypto');
const [sourcePath, outputPath, sharpPath] = process.argv.slice(2);
if (!sourcePath || !outputPath || !sharpPath) {
    throw new Error('Expected master SVG, output directory, and installed sharp module path');
}
const sharp = require(path.resolve(sharpPath));
sharp.concurrency(1);
sharp.cache(false);
const sizes = [16, 24, 32, 48, 64, 128, 256];
const digest = bytes => crypto.createHash('sha256').update(bytes).digest('hex');

async function main() {
    const source = fs.readFileSync(sourcePath);
    const original = source.toString('utf8');
    const background = '<g id="background"><rect width="1254" height="1254" fill="url(#background-tone)" /></g>';
    if (!original.includes('viewBox="0 0 1254 1254"') || !original.includes(background)) {
        throw new Error('Unexpected approved master geometry; inspect before exporting');
    }
    const adapted = original.replace('viewBox="0 0 1254 1254"', 'viewBox="78.5 70.5 1097 1097"')
        .replace(background, background.replace('<rect ', '<rect clip-path="url(#frame-clip)" '));
    fs.mkdirSync(outputPath, { recursive: true });
    const exported = [];
    const pngs = [];
    for (const size of [...sizes, 512]) {
        const png = await sharp(Buffer.from(adapted)).resize(size, size).png().toBuffer();
        const name = size === 512 ? 'audastra-512.png' : `icon-${size}.png`;
        fs.writeFileSync(path.join(outputPath, name), png);
        exported.push({ file: name, width: size, height: size, bytes: png.length, sha256: digest(png) });
        if (size !== 512) pngs.push({ size, png });
    }
    const header = Buffer.alloc(6 + 16 * pngs.length);
    header.writeUInt16LE(1, 2);
    header.writeUInt16LE(pngs.length, 4);
    let offset = header.length;
    for (const [i, { size, png }] of pngs.entries()) {
        const p = 6 + i * 16;
        header[p] = size === 256 ? 0 : size;
        header[p + 1] = header[p];
        header.writeUInt16LE(1, p + 4);
        header.writeUInt16LE(32, p + 6);
        header.writeUInt32LE(png.length, p + 8);
        header.writeUInt32LE(offset, p + 12);
        offset += png.length;
    }
    const ico = Buffer.concat([header, ...pngs.map(({ png }) => png)]);
    fs.writeFileSync(path.join(outputPath, 'audastra.ico'), ico);
    exported.push({ file: 'audastra.ico', sizes, bytes: ico.length, sha256: digest(ico) });
    const provenance = {
        schema: 1, master: 'audastra.svg', master_sha256: digest(source),
        approval: 'Author approved geometric revision 2 in this conversation on 2026-10-05',
        adaptation: { viewBox: [78.5, 70.5, 1097, 1097], background_clip: 'frame-clip',
            geometry: 'All approved paths, colors, and proportions preserved' },
        renderer: { sharp: sharp.versions.sharp, vips: sharp.versions.vips, workers: 1 },
        exports: exported
    };
    fs.writeFileSync(path.join(outputPath, 'provenance.json'), JSON.stringify(provenance, null, 2) + '\n');
    console.log(JSON.stringify({ master_sha256: provenance.master_sha256, files: exported.length,
        total_bytes: exported.reduce((n, e) => n + e.bytes, 0), ico_sizes: sizes }));
}
main().catch(error => { console.error(error.message); process.exitCode = 1; });
