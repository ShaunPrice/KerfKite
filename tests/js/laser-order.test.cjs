const test=require('node:test'),assert=require('node:assert/strict');
const order=require('../../studio/static/laser-order.js'),cam=require('../../studio/static/offline-cam.js');
const rect=(x,y,w,h)=>[[x,y],[x+w,y],[x+w,y+h],[x,y+h],[x,y]];
const line=x=>[[x,2],[x+1,2]];
const base={mode:'laser',operation:'line',protocol:'grbl',width:200,height:150,feed:400,power:800,passes:1,stock_thickness:5,depth:1,step:1,safe:4,plunge:100};
const op=(paths,opts)=>({...base,paths,...opts});
const job=(ops)=>({...base,paths:ops.flatMap(o=>o.paths),operations:ops});
const all={dedupe:true,nearest:true,inside:true};

test('nearest valid contour runs first across compatible objects and nested cuts precede outlines',()=>{
  const outer=rect(1,1,70,70),middle=rect(10,10,40,40),inner=rect(20,20,2,3),far=line(130);
  const original=job([op([far]),op([outer,middle]),op([inner])]), saved=structuredClone(original);
  const {job:planned,report}=order.plan(original,['a','a','a'],all);
  assert.deepEqual(original,saved);assert.equal(planned.operations.length,1);
  assert.deepEqual(planned.paths,[inner,middle,outer,far]);assert.ok(report.after<report.before);
  const generated=cam.generate(planned);assert.deepEqual(generated.segments[0].slice(0,4),[20,20,22,20]);
});
test('duplicates compare direction and cyclic start, retain near geometry, and require opt in',()=>{
  const a=rect(10,5,6,6),b=[a[2],a[1],a[0],a[3],a[2]],l=line(40);
  const copy=a.map(([x,y])=>[x+1e-6,y]);
  const j=job([op([a,b,copy,l,l.slice().reverse()])]);
  const result=order.plan(j,['a'],all);
  assert.equal(result.report.removed,2);assert.equal(result.job.paths.length,3);
  assert.equal(order.plan(j,['a'],{...all,dedupe:false}).job.paths.length,5);
});
test('raster, settings overrides, fill, multipass and layer boundaries keep their positions and settings',()=>{
  const p=rect(5,5,3,3),raster=op([line(11)],{path_powers:[[0,120]]});
  const original=job([op([p]),raster,op([p]),op([p],{power:70}),op([p],{passes:2}),op([p],{operation:'fill'}),op([p]),op([p])]);
  const result=order.plan(original,['a','a','a','a','a','a','a','b'],all);
  assert.deepEqual(result.job.operations,original.operations);assert.equal(result.report.removed,0);
  assert.equal(result.report.skipped,3);
});
test('concave outline crossing an inner contour cannot create a false containment dependency',()=>{
  const u=[[0,0],[12,0],[12,12],[8,12],[8,4],[4,4],[4,12],[0,12],[0,0]],across=[[2,8],[10,8]];
  const result=order.plan(job([op([u,across])]),['a'],{inside:true});
  assert.deepEqual(result.job.paths,[u,across]);
});
test('complex groups preserve original program ordering and report the fallback',()=>{
  const original=job([op(Array.from({length:201},(_,i)=>line(i/2)))]);
  const result=order.plan(original,['a'],all);
  assert.equal(result.report.oversize,1);assert.deepEqual(result.job,original);
});
