const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');
const root = path.join(__dirname,'../../studio/static');
const scope = vm.createContext({});
for (const script of ['vendor/qrcodegen.js','pairing-qr.js'])
  vm.runInContext(fs.readFileSync(path.join(root,script),'utf8'),scope);
const qr = scope.KerfPairingQr;
const token = 'kT3vYw0Qm1x_Zr8N-5sHc2LbJ7dUeGfPaVo4iWq9XyA';
const pinned = 'https://192.168.1.20:8766/#token='+token+'&fp='+'ab'.repeat(32);
const plain = 'http://192.168.1.20:8766/#token='+token;

// Finder patterns: a 7×7 ring with a 3×3 centre in three corners.
function finder(rows, x0, y0) {
  for (let y=0;y<7;y++) for (let x=0;x<7;x++) {
    const ring = x===0 || y===0 || x===6 || y===6, core = x>1 && x<5 && y>1 && y<5;
    if (rows[y0+y][x0+x] !== (ring || core)) return false;
  }
  return true;
}

test('pairing links become square symbols with the three finder patterns',()=>{
  for (const link of [pinned, plain]) {
    const rows = qr.modules(link), n = rows.length;
    assert.ok(n>=21 && (n-17)%4===0 && rows.every(row=>row.length===n));
    assert.ok(finder(rows,0,0) && finder(rows,n-7,0) && finder(rows,0,n-7));
  }
  assert.ok(qr.modules(pinned).length > qr.modules(plain).length, 'the fingerprint needs a larger symbol');
});
test('the path covers exactly the dark modules, inside a four-module quiet zone',()=>{
  const {size, quiet, path:d, rows} = qr.symbol(pinned);
  assert.equal(quiet,4);
  assert.equal(size,rows.length+8);
  const drawn = Array.from({length:size},()=>Array(size).fill(false));
  for (const [,x,y,run] of d.matchAll(/M(\d+),(\d+)h(\d+)v1h-\d+z/g))
    for (let i=0;i<Number(run);i++) drawn[Number(y)][Number(x)+i] = true;
  for (let y=0;y<size;y++) for (let x=0;x<size;x++) {
    const inside = x>=4 && y>=4 && x<size-4 && y<size-4;
    assert.equal(drawn[y][x], inside && rows[y-4][x-4]);
  }
  assert.equal(d.replace(/M\d+,\d+h\d+v1h-\d+z/g,''),'');
});
test('the drawing is black on white and holds nothing but the symbol',()=>{
  const svg = qr.svg(pinned);
  assert.match(svg,/^<svg [^>]*viewBox="0 0 \d+ \d+"[^>]*><rect [^>]*fill="#fff"\/><path d="[Mhvz0-9,-]+" fill="#000"\/><\/svg>$/);
  assert.ok(!svg.includes(token));
});
test('only pairing links are encoded',()=>{
  for (const bad of ['', 'https://192.168.1.20:8766/', 'ftp://host/#token=x', 'https://host/#token=', 'https://ho st/#token=x', 'x'.repeat(401), null, 'https://host/#token=a b'])
    assert.throws(()=>qr.modules(bad),/Not a pairing link/);
});
