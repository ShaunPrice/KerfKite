const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');
const root = path.join(__dirname,'../../studio/static');
const font = require(path.join(root,'stroke-font.js'));
const scope = vm.createContext({TextEncoder});
for (const script of ['vendor/qrcodegen.js','production-labels.js'])
  vm.runInContext(fs.readFileSync(path.join(root,script),'utf8'),scope);
const labels=scope.KerfLabels;

test('single-line face covers printable ASCII and respects mm, spacing and lifts',()=>{
  for(let c=33;c<127;c++){
    const lines=font.paths(String.fromCharCode(c),6);
    assert.ok(lines.length && lines.every(p=>p.length>1 && p.flat().every(Number.isFinite)));
  }
  assert.equal(font.paths('F',6).length,2);
  const bar=font.paths('A',6,1).at(-1)[0];
  assert.deepEqual(font.paths('A A',6,1).at(-1)[0],[bar[0]+16,bar[1]]);
  assert.throws(()=>font.paths('水',5),/no glyph/);
});
test('serial runs and bar geometry reserve quiet zones and cap complexity',()=>{
  assert.deepEqual(Array.from(labels.sequence('K-{n}',3,7,2,3)),['K-007','K-009','K-011']);
  assert.throws(()=>labels.sequence('K',2,0,1,2),/serial number/);
  const barcode=labels.code39('A');
  assert.equal(barcode.rows[0].length,47); // 3 symbols, 15 modules each, and 2 gaps
  assert.deepEqual(Array.from(barcode.rows[0].slice(16,31),Number),[1,1,1,0,1,0,1,0,0,0,1,0,1,1,1]);
  assert.throws(()=>labels.code39('abc'));
  const symbol=labels.qr('Gear-17');
  assert.equal(symbol.rows.length,21);
  assert.equal(symbol.quietX,4);
  const art=labels.engrave(symbol,1,.25,10,0,0,3000);
  assert.equal(art.width,29);assert.equal(art.height,29);
  assert.ok(art.paths.flat().every(([x,y])=>x>=4 && y>=4 && x<=25 && y<=25));
  assert.throws(()=>labels.engrave(symbol,1,.25,10,0,0,8),/limit/);
});
