const {test}=require('node:test');
const assert=require('node:assert/strict');
const {templates,normalize}=require('../studio/static/templates.js');
test('library has diverse uniquely identified artwork',()=>{assert.equal(templates.length,1000);assert.equal(new Set(templates.map(t=>t.category)).size,52);assert.equal(new Set(templates.map(t=>t.id)).size,templates.length);for(const category of ['Flowers','Vines','Leaves','Symbols','Tags & badges','Frames','World landmarks','Historic buildings','Mountain scenes','River scenes','Animal portraits','Insects','Children’s toy artwork','Learning & play','Cars & road vehicles','Motorbikes & cycles','Musical instruments','Music notation'])assert.ok(templates.some(t=>t.category===category));});
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
test('MCP built-in snapshot matches browser artwork',()=>compareArtwork(require('../studio/builtin_templates.json'),templates));
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
  vm.runInNewContext(fs.readFileSync(require.resolve('../studio/static/templates.js'), 'utf8'), page);
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
