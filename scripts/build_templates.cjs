/* Regenerate the MCP snapshot and human-readable catalogue from browser artwork. */
const fs=require('node:fs'),path=require('node:path');
const root=path.resolve(__dirname,'..');
const source = path.join(root, 'studio/static/templates.js');
const {templates, index} = require(source).buildCatalog();
const start = '// BEGIN GENERATED TEMPLATE INDEX';
const end = '// END GENERATED TEMPLATE INDEX';
const code = fs.readFileSync(source, 'utf8');
const begin = code.indexOf(start), finish = code.indexOf(end);
if (begin < 0 || finish <= begin) throw new Error('Template index markers missing');
const rows = index.map(row => JSON.stringify(row)).join(',\n');
fs.writeFileSync(source, code.slice(0, begin) + start + '\nconst templateIndex = [\n' + rows + '\n];\n' + code.slice(finish));
const categories=[...new Set(templates.map(t=>t.category))].sort();
fs.writeFileSync(path.join(root,'studio/builtin_templates.json'),JSON.stringify(templates));
const table=categories.map(c=>`| ${c} | ${templates.filter(t=>t.category===c).length} |`).join('\n');
fs.writeFileSync(path.join(root,'docs/TEMPLATE-CATALOGUE.md'),`# Template catalogue\n\n${templates.length} original built-in vector templates across ${categories.length} categories. The browser and MCP share the same artwork.\n\nOpen **Templates → Template library**, choose a category or search, then click an image to insert it. Previous/Next pages show 36 previews at a time. Favourites and custom templates remain available.\n\nDownload the [SVG stock library](https://github.com/ShaunPrice/KerfKite/releases/download/v0.2.2/KerfKite-Stock-Artwork.zip) for other editors or an earlier Studio installation. Extract the archive and open **index.html** to browse labelled previews. The catalogue keeps each SVG in its category folder. Closed paths describe outlines or holes. Open strokes are usually engraving details.\n\n![A selection of actual stock vector designs](../studio/static/stock-artwork-preview.png)\n\n| Category | Templates |\n|---|---:|\n${table}\n\nArtwork is original procedural vector line art, included under the project MIT licence. Botanical and animal studies are stylised rather than scientific illustrations. Landmark drawings are artistic interpretations, not measured architectural plans. Existing IDs are preserved.\n\nTemplates contain geometry, not preconfigured cutting jobs. Toy designs and learning boards are scalable craft patterns, not validated mechanical assembly plans or certified children's products. Set dimensions, material thickness, hole sizes and fit allowances before fabrication. Some designs combine closed outlines and open engraving detail; assign appropriate operations. Stencil bridges, one-piece silhouette unions and automatic assembly tolerances are not included. Physical cut quality and suitability require review.\n\nRegenerate this catalogue and the MCP snapshot with \`node scripts/build_templates.cjs\`.\n`);
console.log(`${templates.length} templates; ${categories.length} categories`);
