/* Regenerate the MCP snapshot and human-readable catalogue from browser artwork. */
const fs=require('node:fs'),path=require('node:path');
const root=path.resolve(__dirname,'..');
const source = path.join(root, 'studio/static/templates.js');
const {deflateSync} = require('node:zlib');
const {catalogEntries} = require(source);
const index = [];
const counts = new Map();
const seen = new Set();

// KKART v1: 8-byte signature, individually deflated records, UTF-8 JSON index,
// then uint32 index length. Each record is uint32 path count followed by each
// path's uint32 vertex count and XY float64 pairs. All numbers are little-endian.
function encodePaths(paths) {
    const points = paths.reduce((n, path) => n + path.length, 0);
    if (!paths.length || paths.length > 3000 || points > 100000)
        throw new Error('Template exceeds path or point limits');
    const bytes = Buffer.allocUnsafe(4 + 4 * paths.length + 16 * points);
    let offset = bytes.writeUInt32LE(paths.length);
    for (const path of paths) {
        if (path.length < 2) throw new Error('Template path must have at least two points');
        offset = bytes.writeUInt32LE(path.length, offset);
        for (const point of path) {
            const [x, y] = point;
            if (point.length !== 2 || !point.every(v => Number.isFinite(v) && v >= 0 && v <= 100))
                throw new Error('Invalid template coordinate');
            offset = bytes.writeDoubleLE(x || 0, offset);
            offset = bytes.writeDoubleLE(y || 0, offset);
        }
    }
    return deflateSync(bytes, {level: 9});
}

function writeCatalog(filename) {
    const temporary = fs.mkdtempSync(path.join(path.dirname(filename), '.catalog-'));
    const pending = path.join(temporary, 'pending');
    try {
        const fd = fs.openSync(pending, 'wx');
        try {
            const signature = Buffer.from('KKART\x01\r\n', 'ascii');
            fs.writeFileSync(fd, signature);
            let offset = signature.length;
            const records = [];
            for (const [key, template] of catalogEntries()) {
                const {paths, ...metadata} = template;
                const {id, name, category, aspect} = metadata;
                if (seen.has(id)) throw new Error('Duplicate template ID: ' + id);
                seen.add(id);
                index.push([key, id, name, category, aspect]);
                counts.set(category, (counts.get(category) || 0) + 1);
                const packed = encodePaths(paths);
                records.push([offset, packed.length, metadata]);
                fs.writeFileSync(fd, packed);
                offset += packed.length;
            }
            const table = Buffer.from(JSON.stringify(records));
            fs.writeFileSync(fd, table);
            const length = Buffer.alloc(4);
            length.writeUInt32LE(table.length);
            fs.writeFileSync(fd, length);
        } finally {
            fs.closeSync(fd);
        }
        fs.renameSync(pending, filename);
    } finally {
        fs.rmSync(temporary, {recursive: true, force: true});
    }
}
const start = '// BEGIN GENERATED TEMPLATE INDEX';
const end = '// END GENERATED TEMPLATE INDEX';
const code = fs.readFileSync(source, 'utf8');
const begin = code.indexOf(start), finish = code.indexOf(end);
if (begin < 0 || finish <= begin) throw new Error('Template index markers missing');
writeCatalog(path.join(root, 'studio/builtin_templates.kkart'));
const rows = index.map(row => JSON.stringify(row)).join(',\n');
fs.writeFileSync(source, code.slice(0, begin) + start + '\nconst templateIndex = [\n' + rows + '\n];\n' + code.slice(finish));
const categories = [...counts.keys()].sort();
const table = categories.map(c => `| ${c} | ${counts.get(c)} |`).join('\n');
fs.writeFileSync(path.join(root,'docs/TEMPLATE-CATALOGUE.md'),`# Template catalogue\n\n${index.length} original built-in vector templates across ${categories.length} categories. The browser and MCP share the same artwork.\n\nOpen **Templates → Template library**, choose a category or search, then click an image to insert it. Previous/Next pages show 36 previews at a time. Favourites and custom templates remain available.\n\nDownload the [SVG stock library](https://github.com/ShaunPrice/KerfKite/releases/download/v0.2.2/KerfKite-Stock-Artwork.zip) for other editors or an earlier Studio installation. Extract the archive and open **index.html** to browse labelled previews. The catalogue keeps each SVG in its category folder. Closed paths describe outlines or holes. Open strokes are usually engraving details.\n\n![A selection of actual stock vector designs](../studio/static/stock-artwork-preview.png)\n\n| Category | Templates |\n|---|---:|\n${table}\n\nArtwork is original procedural vector line art, included under the project MIT licence. Botanical and animal studies are stylised rather than scientific illustrations. Landmark drawings are artistic interpretations, not measured architectural plans. Existing IDs are preserved.\n\nTemplates contain geometry, not preconfigured cutting jobs. Toy designs and learning boards are scalable craft patterns, not validated mechanical assembly plans or certified children's products. Set dimensions, material thickness, hole sizes and fit allowances before fabrication. Some designs combine closed outlines and open engraving detail; assign appropriate operations. Stencil bridges, one-piece silhouette unions and automatic assembly tolerances are not included. Physical cut quality and suitability require review.\n\nMCP searches the bundled index and loads geometry when you select a template. Offline SVG export uses the same indexed artwork. Regenerate the catalogue and its packed geometry with \`node scripts/build_templates.cjs\`.\n`);
console.log(`${index.length} templates; ${categories.length} categories`);
