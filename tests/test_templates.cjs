const {test}=require('node:test');
const assert=require('node:assert/strict');
const {templates,normalize}=require('../studio/static/templates.js');
test('library has diverse uniquely identified artwork',()=>{assert.ok(templates.length >= 1500);assert.ok(new Set(templates.map(t=>t.category)).size >= 100);assert.equal(new Set(templates.map(t=>t.id)).size,templates.length);for(const category of ['Flowers','Vines','Leaves','Symbols','Tags & badges','Frames','World landmarks','Historic buildings','Mountain scenes','River scenes','Animal portraits','Insects','Children’s toy artwork','Learning & play','Cars & road vehicles','Motorbikes & cycles','Musical instruments','Music notation'])assert.ok(templates.some(t=>t.category===category));});
test('all templates have finite bounded geometry with drawable segments',()=>{for(const t of templates){assert.ok(t.paths.length>0,t.name);for(const p of t.paths){assert.ok(p.length>=2,t.name);for(const point of p){assert.equal(point.length,2);for(const n of point)assert.ok(Number.isFinite(n)&&n>=0&&n<=100,t.name);}}}});
test('normalization preserves open versus closed paths and relative positioning',()=>{const result=normalize([[[10,20],[20,20],[20,40],[10,20]],[[15,30],[18,35]]]);assert.deepEqual(result[0][0],result[0].at(-1));assert.notDeepEqual(result[1][0],result[1].at(-1));assert.deepEqual(result[1][0],[50,50]);});
// Trigonometric results vary by a few ULPs across CPU/JS runtimes.
// Keep topology and metadata exact; allow only negligible coordinate error.
function compareArtwork(actual,expected,path='templates') {
  if(typeof expected==='number') {
    assert.ok(Number.isFinite(actual)&&Math.abs(actual-expected)<=1e-10,`${path}: ${actual} != ${expected}`);
  } else if(Array.isArray(expected)) {
    assert.ok(Array.isArray(actual),path);assert.equal(actual.length,expected.length,path);
    expected.forEach((value,index)=>compareArtwork(actual[index],value,`${path}[${index}]`));
  } else if(expected&&typeof expected==='object') {
    assert.deepEqual(Object.keys(actual).sort(),Object.keys(expected).sort(),path);
    for(const key of Object.keys(expected))compareArtwork(actual[key],expected[key],`${path}.${key}`);
  } else assert.equal(actual,expected,path);
}
// The packed catalogue that kerfkite-mcp and the SVG export read (scripts/build_templates.cjs): the magic, each
// template's zlib-compressed paths (a count, then each path's point count and little-endian double pairs), and a
// JSON index of [offset, length, metadata] records whose byte length ends the file.
function packedCatalog(file) {
  const data = require('node:fs').readFileSync(file), zlib = require('node:zlib');
  assert.equal(data.subarray(0, 8).toString('latin1'), 'KKART\x01\r\n');
  const size = data.readUInt32LE(data.length - 4), start = data.length - 4 - size;
  assert.ok(start >= 8, 'Invalid artwork catalog index');
  return JSON.parse(data.subarray(start, data.length - 4).toString('utf8')).map(([offset, length, metadata]) => {
    const packed = zlib.inflateSync(data.subarray(offset, offset + length)), paths = [];
    let position = 4;
    for (let count = packed.readUInt32LE(0); paths.length < count;) {
      const points = packed.readUInt32LE(position), path = [];
      position += 4;
      assert.ok(position + 16 * points <= packed.length, 'Truncated template path');
      for (let i = 0; i < points; i++, position += 16) path.push([packed.readDoubleLE(position), packed.readDoubleLE(position + 8)]);
      paths.push(path);
    }
    assert.equal(position, packed.length, 'Unexpected data after template paths');
    return {...metadata, paths};
  });
}
test('packed MCP and SVG catalog matches every browser template', () => {
  const packed = packedCatalog(require('node:path').resolve(__dirname, '../studio/builtin_templates.kkart'));
  assert.equal(packed.length, templates.length);
  packed.forEach((item, count) => compareArtwork(item, templates[count], templates[count].id));
});
test('artwork comparison rejects changed geometry and topology',()=>{
  assert.throws(()=>compareArtwork([[1,2]],[[1,2.000001]]));
  assert.throws(()=>compareArtwork([[1,2]],[[1,2],[3,4]]));
  assert.throws(()=>compareArtwork({name:'vine'},{name:'flower'}));
});

const {clip,inside}=require('../studio/static/ornaments.js');
test('occlusion splits stems at leaf boundary in exported geometry',()=>{const mask=[[2,-1],[4,-1],[4,1],[2,1],[2,-1]],p=clip([[[0,0],[6,0]]],mask);assert.deepEqual(p,[[[0,0],[2,0]],[[4,0],[6,0]]]);for(const path of p)for(let i=1;i<path.length;i++)assert.equal(inside([(path[i-1][0]+path[i][0])/2,0],mask),false);});

test('expanded library is distinct, named and within insertion budgets',()=>{const seen=new Set();for(const t of templates){assert.ok(t.name.length<=80);assert.ok(t.category.length<=40);assert.ok(t.aspect>0&&Number.isFinite(t.aspect));assert.ok(t.paths.length<=3000);assert.ok(t.paths.reduce((n,p)=>n+p.length,0)<=100000);const key=JSON.stringify([+t.aspect.toFixed(7),t.paths.map(p=>p.map(v=>v.map(x=>+x.toFixed(7))))]);assert.ok(!seen.has(key),'Duplicate artwork: '+t.name);seen.add(key);}});

test('classic-script editor browses all metadata without generating artwork', () => {
  const vm = require('node:vm');
  const fs = require('node:fs');
  let artisanBuilds = 0;
  const ornaments = require('../studio/static/ornaments.js');
  const observed = Object.create(ornaments, {
    designs: {get() { artisanBuilds++; return ornaments.designs; }}
  });
  const page = {CNCOrnaments: observed};
  const entry = fs.readFileSync(require.resolve('../studio/static/index.html'), 'utf8');
  for (const match of entry.matchAll(/<script src="\.\/(stock-[\w-]+\.js|templates\.js)"><\/script>/g)) {
    vm.runInNewContext(fs.readFileSync(require.resolve('../studio/static/' + match[1]), 'utf8'), page);
  }
  const items = page.CNCTemplates.templates;
  assert.equal(items.filter(item => item.category === 'World landmarks').length, 12);
  assert.equal(artisanBuilds, 0);
  const landmark = items.find(item => item.name.includes('Sydney Opera House'));
  assert.ok(landmark.paths.length);
  assert.equal(artisanBuilds, 0);
  assert.ok(items[0].paths.length);
  assert.equal(artisanBuilds, 1);
  assert.ok(items[1].paths.length);
  assert.equal(artisanBuilds, 1);
});

test('stock artwork produces router, laser and knife contour previews at its native aspect', () => {
  const {generate} = require('../studio/static/offline-cam.js');
  for (const item of templates.filter(t => t.id.startsWith('stock-'))) {
    const w = Math.min(1, item.aspect), h = Math.min(1, 1 / item.aspect);
    const paths = item.paths.map(p => p.map(([x, y]) => [20 + x*w, 20 + y*h]));
    for (const mode of ['laser', 'router', 'knife']) {
      const job = generate({mode, operation:'line', width:160, height:160, paths,
        feed:600, plunge:100, power:650, safe:3, passes:1, depth:1, step:1});
      assert.ok(job.segments.length, item.id + ' in ' + mode);
    }
  }
});
