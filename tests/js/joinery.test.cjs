const test=require('node:test'),assert=require('node:assert/strict');
const {dogbone,nest,area}=require('../../studio/static/joinery.js');
const square=[[0,0],[20,0],[20,20],[0,20],[0,0]];
// An L-shaped part: five convex corners and one that turns into the part, at (10,10).
const ell=[[0,0],[30,0],[30,10],[10,10],[10,30],[0,30],[0,0]];
const inside=(path,p)=>{let c=false;for(let i=0,j=path.length-1;i<path.length;j=i++){const [xi,yi]=path[i],[xj,yj]=path[j];if((yi>p[1])!==(yj>p[1])&&p[0]<(xj-xi)*(p[1]-yi)/(yj-yi)+xi)c=!c;}return c;};

test('a slot gets a relief at each corner whose circle passes through the corner point',()=>{
  const r=1.6,out=dogbone(square,r,{side:'inside'});
  assert.equal(out.corners,4);assert.equal(out.skipped,0);
  assert.deepEqual(out.path[0],out.path.at(-1));
  // The slot grows by four slivers outside the square: each is a circular segment beyond two edges.
  assert.ok(area(out.path)>400&&area(out.path)<400+4*Math.PI*r*r);
  for(const corner of square.slice(0,4)){
    assert.ok(out.path.some(p=>Math.hypot(p[0]-corner[0],p[1]-corner[1])<1e-9),'the corner point stays on the outline');
    // The cutter centre that reaches the corner: on the bisector, one radius from the corner.
    const c=[corner[0]+(corner[0]?-1:1)*r/Math.SQRT2,corner[1]+(corner[1]?-1:1)*r/Math.SQRT2];
    // Every outline point is at least a radius from that centre: a cutter of that radius fits there.
    for(const p of out.path)assert.ok(Math.hypot(p[0]-c[0],p[1]-c[1])>=r-1e-9);
    // The relief leaves each edge 2·r·cos(45°) from the corner.
    const reach=2*r*Math.SQRT1_2;
    assert.ok(out.path.some(p=>Math.abs(Math.hypot(p[0]-corner[0],p[1]-corner[1])-reach)<1e-9));
  }
});
test('a part cut round the outside is relieved only where the outline turns into it',()=>{
  const out=dogbone(ell,1.6,{side:'outside'});
  assert.equal(out.corners,1);
  // The relief cuts into the part, so the part loses area; the other corners are untouched.
  assert.ok(area(out.path)<area(ell)&&area(out.path)>area(ell)-Math.PI*1.6*1.6);
  for(const corner of ell.slice(0,6))assert.ok(out.path.some(p=>p[0]===corner[0]&&p[1]===corner[1]));
  assert.equal(dogbone(ell,1.6,{side:'inside'}).corners,5);
  assert.equal(dogbone(square,1.6,{side:'outside'}).corners,0);
});
test('the direction an outline is drawn in does not matter',()=>{
  const reversed=[...ell].reverse();
  assert.equal(dogbone(reversed,1.6,{side:'outside'}).corners,1);
  assert.equal(dogbone(reversed,1.6,{side:'inside'}).corners,5);
  assert.ok(Math.abs(Math.abs(area(dogbone(reversed,1.6,{side:'inside'}).path))-Math.abs(area(dogbone(ell,1.6,{side:'inside'}).path)))<1e-9);
});
test('an edge too short for the relief is left alone and counted',()=>{
  const small=[[0,0],[4,0],[4,4],[0,4],[0,0]];
  const out=dogbone(small,1.6,{side:'inside'});  // each relief would take 2.26 mm of a 4 mm edge
  assert.equal(out.corners,0);assert.equal(out.skipped,4);
  assert.deepEqual(out.path,small);
});
test('nearly straight corners and repeated points are ignored',()=>{
  const bent=[[0,0],[10,0],[20,.2],[20,20],[0,20],[0,0],[0,0]];
  const out=dogbone([[0,0],[10,0],[10,0],[20,.2],[20,20],[0,20],[0,0]],1,{side:'inside'});
  assert.equal(out.corners,4);
  assert.throws(()=>dogbone(bent.slice(0,3),1),/closed/);
});
test('requests are validated',()=>{
  assert.throws(()=>dogbone([[0,0],[10,0],[10,10],[0,10]],1),/closed outlines/);
  assert.throws(()=>dogbone(square,0),/radius must be between/);
  assert.throws(()=>dogbone(square,1,{side:'left'}),/inside or outside/);
  assert.throws(()=>dogbone([[0,0],[1,NaN],[2,2],[0,0]],1),/invalid coordinates/);
});
test('parts are nested in rows without overlap, inside the sheet',()=>{
  const sizes=[{w:40,h:30},{w:60,h:20},{w:20,h:50},{w:35,h:35},{w:80,h:10},{w:25,h:25}];
  const {placements,used}=nest(sizes,150,120,4);
  assert.equal(placements.length,sizes.length);
  assert.deepEqual(placements.map(p=>p.index),[0,1,2,3,4,5]);
  for(const p of placements){
    assert.ok(p.x>=0&&p.y>=0&&p.x+p.w<=150+1e-9&&p.y+p.h<=120+1e-9);
    assert.equal(p.w,sizes[p.index].w);assert.equal(p.turned,false);
  }
  for(const a of placements)for(const b of placements)if(a.index<b.index){
    const apart=a.x+a.w+4<=b.x+1e-9||b.x+b.w+4<=a.x+1e-9||a.y+a.h+4<=b.y+1e-9||b.y+b.h+4<=a.y+1e-9;
    assert.ok(apart,`parts ${a.index} and ${b.index} keep the gap`);
  }
  assert.ok(used[0]<=150&&used[1]<=120);
  assert.deepEqual(nest([{w:10,h:10}],10,10,0).placements[0],{index:0,x:0,y:0,w:10,h:10,turned:false});
});
test('nesting refuses what cannot fit, and turns a part only when allowed',()=>{
  assert.throws(()=>nest([{w:200,h:10}],150,120,4),/larger than the sheet/);
  assert.throws(()=>nest(Array.from({length:30},()=>({w:50,h:50})),150,120,4),/do not fit the sheet/);
  assert.throws(()=>nest([],150,120),/1 to 500 parts/);
  assert.throws(()=>nest([{w:1,h:1}],0,10),/valid sheet size/);
  assert.throws(()=>nest([{w:1,h:1}],10,10,-1),/gap between parts/);
  const turned=nest([{w:10,h:140}],150,120,4,{rotate:true}).placements[0];
  assert.deepEqual([turned.w,turned.h,turned.turned],[140,10,true]);
});
