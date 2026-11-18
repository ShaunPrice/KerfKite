const test=require('node:test');
const assert=require('node:assert/strict');
const trace=require('../studio/static/camera-trace.js');

test('solid rectangle simplifies to four corners and closes exactly',()=>{
 const paths=trace.contours(new Uint8Array(12).fill(1),4,3,0);
 assert.equal(paths.length,1);assert.equal(paths[0].length,5);
 assert.deepEqual(paths[0][0],paths[0].at(-1));assert.equal(Math.abs(trace.signedArea(paths[0])),12);
});
test('holes survive tracing with opposite orientation',()=>{
 const mask=new Uint8Array(25).fill(1);mask[12]=0;
 const paths=trace.contours(mask,5,5,0);
 assert.equal(paths.length,2);assert.equal(paths.reduce((s,p)=>s+trace.signedArea(p),0),24);
 assert.equal(paths.filter(p=>trace.signedArea(p)<0).length,1);
});
test('diagonal contacts remain separate closed contours',()=>{
 const paths=trace.contours(Uint8Array.from([1,0,0,1]),2,2,0);
 assert.equal(paths.length,2);assert.deepEqual(paths.map(p=>Math.abs(trace.signedArea(p))),[1,1]);
 for(const p of paths)assert.equal(p.length,5);
});
test('minimum feature size removes speckles and explicitly filters tiny holes',()=>{
 const mask=new Uint8Array(25).fill(1);mask[12]=0;
 const paths=trace.contours(mask,5,5,4);assert.equal(paths.length,1);assert.equal(Math.abs(trace.signedArea(paths[0])),25);
 assert.deepEqual(trace.contours(Uint8Array.from([1,0,0,0]),2,2,4),[]);
});
test('threshold composites transparent pixels on white and supports inversion',()=>{
 const rgba=Uint8Array.from([0,0,0,255,0,0,0,0,255,255,255,255,127,127,127,255]);
 assert.deepEqual([...trace.threshold(rgba,4,1,128)], [1,0,0,1]);
 assert.deepEqual([...trace.threshold(rgba,4,1,128,true)], [0,1,1,0]);
});
test('physical mapping preserves whole image dimensions and flips image Y',()=>{
 const paths=trace.mapPaths([[[0,0],[4,0],[4,3],[0,3],[0,0]]],4,3,100,60,10,20);
 assert.deepEqual(paths[0],[[10,80],[110,80],[110,20],[10,20],[10,80]]);
});
test('noise, invalid dimensions and unbounded output are rejected',()=>{
 assert.throws(()=>trace.threshold(new Uint8Array(4),500,1),/384/);
 assert.throws(()=>trace.contours(new Uint8Array(4),2,2,-1),/Minimum/);
 assert.throws(()=>trace.mapPaths([],1,1,-2,3),/positive/);
 const mask=new Uint8Array(384*384);for(let y=0;y<384;y++)for(let x=0;x<384;x++)mask[y*384+x]=(x+y)%2;
 assert.throws(()=>trace.contours(mask,384,384,0),/too noisy/);
});
test('collinear simplification preserves area on irregular stepped silhouettes',()=>{
 const mask=Uint8Array.from([1,1,1,0,1,1,0,0,1,0,0,0]);
 const paths=trace.contours(mask,4,3,0);assert.equal(paths.length,1);
 assert.equal(trace.signedArea(paths[0]),6);
});
test('all 3 by 3 masks preserve exact filled area and closed boundaries',()=>{
 for(let pattern=0;pattern<512;pattern++){
  const mask=Uint8Array.from({length:9},(_,i)=>(pattern>>i)&1),paths=trace.contours(mask,3,3,0);
  assert.equal(paths.reduce((s,p)=>s+trace.signedArea(p),0),mask.reduce((s,n)=>s+n,0));
  for(const p of paths)assert.deepEqual(p[0],p.at(-1));
 }
});
test('a thick straight stroke produces one open centreline',()=>{
 const mask=new Uint8Array(21*11);for(let y=3;y<8;y++)for(let x=2;x<19;x++)mask[y*21+x]=1;
 const original=[...mask],paths=trace.centerlines(mask,21,11,0);
 assert.equal(paths.length,1);assert.equal(paths[0].length,2);
 assert.equal(paths[0][0][1],5.5);assert.equal(paths[0][1][1],5.5);
 assert.notDeepEqual(paths[0][0],paths[0].at(-1));assert.deepEqual([...mask],original);
 assert.deepEqual(trace.centerlines(mask,21,11,0),paths);
 const mapped=trace.mapPaths(paths,21,11,210,110,10,20);
 assert.notDeepEqual(mapped[0][0],mapped[0].at(-1));
});
test('skeleton junctions split into open paths meeting at a shared vertex',()=>{
 const mask=new Uint8Array(15*15);for(let y=2;y<13;y++)mask[y*15+7]=1;for(let x=2;x<13;x++)mask[7*15+x]=1;
 const paths=trace.centerlines(mask,15,15,0);assert.equal(paths.length,4);
 for(const p of paths){assert.equal(p.length,2);assert.notDeepEqual(p[0],p.at(-1));assert.ok(p.some(([x,y])=>x===7.5&&y===7.5));}
});
test('actual skeleton loops remain closed and short strokes can be filtered',()=>{
 const mask=new Uint8Array(12*12);for(let y=2;y<=9;y++)for(let x=2;x<=9;x++)if(x===2||x===9||y===2||y===9)mask[y*12+x]=1;
 const paths=trace.centerlines(mask,12,12,0);assert.equal(paths.length,1);assert.deepEqual(paths[0][0],paths[0].at(-1));
 assert.deepEqual(trace.centerlines(Uint8Array.from([1,1,1]),3,1,4),[]);
});
test('centreline input and output budgets fail explicitly',()=>{
 assert.throws(()=>trace.centerlines(new Uint8Array(400),400,1,0),/mask size/);
 assert.throws(()=>trace.centerlines(new Uint8Array(4),2,2,-1),/stroke length/);
 const mask=new Uint8Array(384*384);for(let y=0;y<384;y++)for(let x=0;x<384;x++)mask[y*384+x]=(x+y)%2;
 assert.throws(()=>trace.centerlines(mask,384,384,0),/too noisy/);
});
