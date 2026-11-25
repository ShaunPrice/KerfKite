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

// ---- nesting by outline ----
const {nestShapes,vInlay}=require('../../studio/static/joinery.js');
const bracket=s=>[[0,0],[3*s,0],[3*s,s],[s,s],[s,3*s],[0,3*s],[0,0]];
const placed=(part,p)=>part.paths.map(path=>path.map(([x,y])=>[p.transform[0]*x-p.transform[1]*y+p.transform[2],p.transform[1]*x+p.transform[0]*y+p.transform[3]]));
const segmentDistance=(a,b,c,d)=>{
  const point=(p,q,r)=>{const l=(r[0]-q[0])**2+(r[1]-q[1])**2,t=l?Math.max(0,Math.min(1,((p[0]-q[0])*(r[0]-q[0])+(p[1]-q[1])*(r[1]-q[1]))/l)):0;return Math.hypot(p[0]-q[0]-t*(r[0]-q[0]),p[1]-q[1]-t*(r[1]-q[1]));};
  const side=(p,q,r)=>Math.sign((q[0]-p[0])*(r[1]-p[1])-(q[1]-p[1])*(r[0]-p[0]));
  if(side(a,b,c)!==side(a,b,d)&&side(c,d,a)!==side(c,d,b))return 0;
  return Math.min(point(a,c,d),point(b,c,d),point(c,a,b),point(d,a,b));
};
// The least distance between two placed parts, by their own edges; 0 when one lies inside the other.
const apart=(one,two)=>{
  let least=Infinity;
  for(const p of one)for(const q of two)for(let i=0;i+1<p.length;i++)for(let j=0;j+1<q.length;j++)least=Math.min(least,segmentDistance(p[i],p[i+1],q[j],q[j+1]));
  for(const [m,n] of [[one,two],[two,one]])if(n[0].length>3&&inside(n[0],m[0][0]))return 0;
  return least;
};
const check=(parts,result,width,height,gap)=>{
  const shapes=result.placements.map(p=>placed(parts[p.index],p));
  for(const shape of shapes)for(const path of shape)for(const [x,y] of path)assert.ok(x>=-1e-9&&y>=-1e-9&&x<=width+1e-9&&y<=height+1e-9,'a part hangs off the sheet');
  for(let i=0;i<shapes.length;i++)for(let j=i+1;j<shapes.length;j++){const d=apart(shapes[i],shapes[j]);assert.ok(d>=gap-1e-9,`parts ${i+1} and ${j+1} are ${d.toFixed(3)} mm apart, less than the gap`);}
  return shapes;
};
test('two brackets nest into each other when one may be turned',()=>{
  const parts=[{paths:[bracket(20)]},{paths:[bracket(20)]}];
  const upright=nestShapes(parts,300,200,4,{turns:'none'}),turned=nestShapes(parts,300,200,4,{turns:'quarter'});
  check(parts,upright,300,200,4);check(parts,turned,300,200,4);
  assert.deepEqual(upright.placements.map(p=>p.angle),[0,0]);
  assert.deepEqual(turned.placements.map(p=>p.angle),[0,180]);
  // Upright they sit side by side: two widths and the gap. Turned, the second sits in the hollow of the first.
  assert.ok(upright.used[0]>=124-1e-9);
  assert.ok(turned.used[0]<95,`turned they take ${turned.used[0]} mm`);
  assert.ok(turned.used[1]<=60+1e-9);
  // The turn is about the part's own origin and then a move: lengths are kept.
  const [,second]=turned.placements;
  assert.ok(Math.abs(Math.hypot(second.transform[0],second.transform[1])-1)<1e-12);
});
test('every part keeps the gap and stays on the sheet, whatever the turns',()=>{
  const triangle=[[0,0],[40,0],[0,30],[0,0]],strip=[[0,0],[90,0],[90,12],[0,12],[0,0]],ring=[[0,0],[50,0],[50,50],[0,50],[0,0]],hole=[[15,15],[35,15],[35,35],[15,35],[15,15]];
  const parts=[{paths:[bracket(15)]},{paths:[triangle]},{paths:[triangle]},{paths:[strip]},{paths:[ring,hole]},{paths:[bracket(10)]},{paths:[[[0,0],[25,18]]]}];
  for(const turns of ['none','quarter',45,15])for(const gap of [0,3,7.5]){
    const result=nestShapes(parts,260,140,gap,{turns});
    assert.equal(result.placements.length,parts.length);
    const shapes=check(parts,result,260,140,gap);
    // Nothing is put in the ring's hole: every other part is outside the ring's outer outline.
    const outer=shapes[4][0];
    shapes.forEach((shape,i)=>{if(i!==4)for(const path of shape)for(const p of path)assert.ok(!inside(outer,p),'a part was placed in a hole');});
    if(turns==='none')assert.ok(result.placements.every(p=>p.angle===0));
  }
});
test('nesting by outline packs tighter than by bounding box',()=>{
  const triangle=[[0,0],[60,0],[0,60],[0,0]],parts=Array.from({length:6},()=>({paths:[triangle]}));
  const shapes=nestShapes(parts,400,300,2,{turns:'quarter'}),boxes=nest(parts.map(()=>({w:60,h:60})),400,300,2);
  check(parts,shapes,400,300,2);
  assert.ok(shapes.used[0]*shapes.used[1]<0.8*boxes.used[0]*boxes.used[1],`${shapes.used} against ${boxes.used}`);
});
test('a fixed part is not turned, and a part that needs turning to fit is turned',()=>{
  const long=[[0,0],[150,0],[150,20],[0,20],[0,0]];
  assert.equal(nestShapes([{paths:[long]}],100,200,2,{turns:'quarter'}).placements[0].angle%180,90);
  assert.throws(()=>nestShapes([{paths:[long],fixed:true}],100,200,2,{turns:'quarter'}),/Part 1 is larger than the sheet/);
  assert.throws(()=>nestShapes([{paths:[long]}],100,200,2,{turns:'none'}),/Part 1 is larger than the sheet/);
  const fixed=nestShapes([{paths:[bracket(20)],fixed:true},{paths:[bracket(20)],fixed:true}],300,200,4,{turns:'quarter'});
  assert.deepEqual(fixed.placements.map(p=>p.angle),[0,0]);
});
test('nesting refuses what cannot be nested',()=>{
  const square=s=>[[0,0],[s,0],[s,s],[0,s],[0,0]];
  assert.throws(()=>nestShapes([{paths:[square(60)]},{paths:[square(60)]}],100,100,2),/do not fit the sheet/);
  assert.throws(()=>nestShapes([],100,100,2),/1 to 200 parts/);
  assert.throws(()=>nestShapes([{paths:[]}],100,100,2),/Part 1 has no outline/);
  assert.throws(()=>nestShapes([{paths:[[[0,0],[NaN,1]]]}],100,100,2),/invalid coordinates/);
  assert.throws(()=>nestShapes([{paths:[square(10)]}],100,100,-1),/gap between parts/);
  assert.throws(()=>nestShapes([{paths:[square(10)]}],100,100,2,{turns:7}),/divides a full turn/);
  assert.throws(()=>nestShapes([{paths:[square(10)]}],0,100,2),/valid sheet size/);
});

// ---- V inlay sizes ----
test('a V inlay is sized from the bit and the depths',()=>{
  const size=vInlay(60,12.7,4,3,1.5,.2),slope=Math.tan(Math.PI/6);
  assert.ok(Math.abs(size.slope-slope)<1e-12);
  assert.equal(size.glue,1);
  // The plug's outline is carved one sampling step ÷ slope deeper than it goes in: the pocket's wall stands that proud.
  assert.equal(size.plugStart,Number((3+.2/slope).toFixed(3)));
  assert.equal(size.plugDepth,Number((size.plugStart+1.5).toFixed(3)));
  assert.ok(Math.abs(size.pocketInset-(4*slope+.2))<1e-12&&Math.abs(size.plugOutset-(1.5*slope+.2))<1e-12);
  assert.throws(()=>vInlay(60,12.7,4,3.95,1.5),/gap for glue/);
  assert.throws(()=>vInlay(60,4,4,3,1.5),/5\.60 mm wide at the top/);
  assert.throws(()=>vInlay(90,12.7,4,3,4),/use a wider bit/);
  assert.throws(()=>vInlay(5,12.7,4,3,1.5),/V-bit angle/);
  assert.throws(()=>vInlay(60,12.7,4,3,1.5,.05),/V detail sampling/);
});
