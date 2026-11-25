const test=require('node:test'),assert=require('node:assert/strict');
const {box,dividers,sign,panel,fingers}=require('../../studio/static/project-wizards.js');
const {area}=require('../../studio/static/joinery.js');
const closed=path=>assert.deepEqual(path[0],path.at(-1));
const extent=path=>[Math.min(...path.map(p=>p[0])),Math.min(...path.map(p=>p[1])),Math.max(...path.map(p=>p[0])),Math.max(...path.map(p=>p[1]))];
const rightAngles=path=>{for(let i=0;i<path.length-1;i++){const a=path[i],b=path[i+1];assert.ok(Math.abs(a[0]-b[0])<1e-9||Math.abs(a[1]-b[1])<1e-9,'edges are horizontal or vertical');}};

test('the panels of a box hold exactly the material of its shell',()=>{
  // Every corner block and every finger belongs to exactly one panel, so the panel areas times the thickness
  // are the volume of the shell: the outside volume less the cavity.
  for(const [width,depth,height,thickness,finger,lid] of [[120,80,60,6,18,false],[120,80,60,6,18,true],[100,100,100,3,10,false],[250,90,45,9,30,true],[61,47,33,4,9,false]]){
    const made=box({width,depth,height,thickness,finger,lid});
    assert.equal(made.parts.length,lid?6:5);
    const material=made.parts.reduce((sum,p)=>sum+Math.abs(area(p.path)),0)*thickness;
    const cavity=(width-2*thickness)*(depth-2*thickness)*(height-(lid?2:1)*thickness);
    assert.ok(Math.abs(material-(width*depth*height-cavity))<1e-6,`${width}×${depth}×${height}: ${material}`);
    assert.deepEqual(made.inside,[width-2*thickness,depth-2*thickness,height-(lid?2:1)*thickness]);
    for(const p of made.parts){closed(p.path);rightAngles(p.path);assert.deepEqual(extent(p.path),[0,0,...p.size]);}
  }
});
test('mating edges are complementary: where one panel has a tab the other has a slot',()=>{
  const t=6,made=box({width:120,depth:80,height:60,thickness:t,finger:18});
  const [bottom,front,,left]=made.parts.map(p=>p.path);
  // Material at depth t/2 inside an edge, sampled along it.
  const solid=(path,x,y)=>{let c=false;for(let i=0,j=path.length-1;i<path.length;j=i++){const [xi,yi]=path[i],[xj,yj]=path[j];if((yi>y)!==(yj>y)&&x<(xj-xi)*(y-yi)/(yj-yi)+xi)c=!c;}return c;};
  let tabs=0;
  for(let x=.5;x<120;x+=1){const b=solid(bottom,x,t/2),f=solid(front,x,t/2);assert.notEqual(b,f,`bottom and front at x=${x}`);tabs+=b;}
  assert.ok(tabs>30&&tabs<90);
  for(let y=.5;y<80;y+=1)assert.notEqual(solid(bottom,t/2,y),solid(left,y,t/2),`bottom and left at y=${y}`);
  // Up a corner: the front's side edge against the left side's edge, above the bottom's corner block.
  for(let z=t+.5;z<60;z+=1)assert.notEqual(solid(front,t/2,z),solid(left,t/2,z),`front and left at z=${z}`);
  // The corner block at the bottom belongs to the bottom alone.
  assert.equal(solid(bottom,t/2,t/2),true);assert.equal(solid(front,t/2,t/2),false);assert.equal(solid(left,t/2,t/2),false);
});
test('the fit allowance narrows every tab and widens every slot by that much on each side',()=>{
  const plain=box({width:120,depth:80,height:60,thickness:6,finger:18}),loose=box({width:120,depth:80,height:60,thickness:6,finger:18,fit:.1});
  // Each boundary between a tab and a slot moves 0.05 mm into the tab, on every panel.
  const boundaries=made=>made.parts.reduce((n,p)=>n+(p.path.length-5)/2,0);
  const lost=plain.parts.reduce((s,p)=>s+Math.abs(area(p.path)),0)-loose.parts.reduce((s,p)=>s+Math.abs(area(p.path)),0);
  assert.ok(Math.abs(lost-boundaries(plain)*.05*6)<1e-6,`${lost}`);
  assert.equal(boundaries(plain),boundaries(loose));
});
test('fingers are odd in number and near the width asked for',()=>{
  assert.equal(fingers(120,18),7);assert.equal(fingers(80,18),5);assert.equal(fingers(60,18),3);assert.equal(fingers(10,18),3);assert.equal(fingers(72,18),5);
  assert.deepEqual(box({width:120,depth:80,height:60,thickness:6,finger:18}).fingers,{width:7,depth:5,height:3});
  // A plain rectangle when no edge is fingered.
  assert.deepEqual(panel(10,5,2,[null,null,null,null]),[[0,0],[10,0],[10,5],[0,5],[0,0]]);
});
test('a box is refused when it cannot be made',()=>{
  assert.throws(()=>box({width:10,depth:80,height:60,thickness:6}),/Box width must be between 18 and 2000/);
  assert.throws(()=>box({width:120,depth:80,height:60,thickness:0}),/Material thickness/);
  assert.throws(()=>box({width:120,depth:80,height:20,thickness:6,finger:6}),/narrower than the material is thick|Box height/);
  assert.throws(()=>box({width:120,depth:80,height:60,thickness:6,finger:2}),/Finger width must be between 6 and 500/);
  assert.throws(()=>box({width:'wide',depth:80,height:60,thickness:6}),/Box width/);
});
test('dividers slot together at half height and leave equal compartments',()=>{
  const made=dividers({width:200,depth:150,height:40,thickness:3,columns:3,rows:2});
  assert.equal(made.parts.length,3);  // one across, two deep
  assert.deepEqual(made.compartment,[(200-2*3)/3,(150-3)/2]);
  const [across,deep]=[made.parts[0].path,made.parts[1].path];
  closed(across);closed(deep);rightAngles(across);rightAngles(deep);
  assert.deepEqual(extent(across),[0,0,200,40]);assert.deepEqual(extent(deep),[0,0,150,40]);
  // Two slots 3 × 20 in the strip across, from the top; one in each strip deep, from the bottom.
  assert.ok(Math.abs(Math.abs(area(across))-(200*40-2*3*20))<1e-9);
  assert.ok(Math.abs(Math.abs(area(deep))-(150*40-3*20))<1e-9);
  assert.ok(across.some(p=>p[1]===20&&Math.abs(p[0]-(200/3-1.5))<1e-9));
  assert.ok(deep.some(p=>p[1]===20&&Math.abs(p[0]-(75+1.5))<1e-9));
  const loose=dividers({width:200,depth:150,height:40,thickness:3,columns:3,rows:2,fit:.2});
  assert.ok(Math.abs(Math.abs(area(loose.parts[0].path))-(200*40-2*3.2*20))<1e-9);
  assert.throws(()=>dividers({width:200,depth:150,height:40,thickness:3,columns:1,rows:1}),/needs no dividers/);
  assert.throws(()=>dividers({width:20,depth:150,height:40,thickness:6,columns:4,rows:2}),/too small for this material/);
  assert.throws(()=>dividers({width:200,depth:150,height:40,thickness:3,columns:2.5,rows:2}),/whole number/);
});
test('a sign blank has rounded corners and two hanging holes',()=>{
  const made=sign({width:200,height:100,radius:10,hole:5,inset:12});
  closed(made.outline);
  assert.deepEqual(extent(made.outline).map(v=>Math.round(v*1e9)/1e9),[0,0,200,100]);
  // A rounded rectangle: the rectangle less four corner squares plus four quarter circles (as polygons, a little less).
  const exact=200*100-(4-Math.PI)*100,got=Math.abs(area(made.outline));
  assert.ok(got<=exact+1e-9&&got>exact-.6,`${got} against ${exact}`);
  assert.equal(made.holes.length,2);
  for(const [hole,x] of [[made.holes[0],12],[made.holes[1],188]]){
    closed(hole.map(p=>p.map(v=>Math.round(v*1e9)/1e9)));
    for(const p of hole)assert.ok(Math.abs(Math.hypot(p[0]-x,p[1]-88)-2.5)<1e-9);
  }
  assert.deepEqual(sign({width:50,height:20}).outline,[[0,0],[50,0],[50,20],[0,20],[0,0]]);
  assert.deepEqual(sign({width:50,height:20}).holes,[]);
  assert.throws(()=>sign({width:50,height:20,radius:11}),/Corner radius must be between 0 and 10/);
  assert.throws(()=>sign({width:50,height:20,hole:5,inset:1}),/Hole inset/);
});
