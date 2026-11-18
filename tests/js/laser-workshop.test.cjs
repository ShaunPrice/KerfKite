const test=require('node:test');
const assert=require('node:assert/strict');
const workshop=require('../../studio/static/laser-workshop.js');
const layers=require('../../studio/static/design-layers.js');
const cam=require('../../studio/static/offline-cam.js');

const defaults={x:2,y:2,width:4,height:2,interval:1,overscan:.5,
  brightness:0,contrast:0,gamma:1,threshold:50,style:'gray',mask:'none',invert:false};
function map(samples,opts={}){
  const pixels=Uint8ClampedArray.from(samples.flatMap(s=>typeof s==='number'?[s,s,s,255]:s));
  return workshop.powerMap(pixels,{...defaults,...opts});
}
const base={mode:'laser',operation:'line',feed:300,power:1000,passes:1,protocol:'grbl',width:200,height:100,stock_thickness:10,depth:1,step:1,safe:5,plunge:100};
const grid={x:1,y:2,rows:2,cols:3,size:4,gap:1,feedMin:300,feedMax:700,powerMin:20,powerMax:60,maxS:1000,pattern:'outline',interval:.5,bedWidth:200,bedHeight:100};

test('source crop and physical sampling dimensions',()=>{
  const rect=workshop.sourceRect(500,200,{left:10,right:30,top:25,bottom:0});
  assert.ok(rect.every((v,i)=>Math.abs(v-[50,50,300,150][i])<1e-9));
  assert.throws(()=>workshop.sourceRect(500,200,{left:75,right:25,top:0,bottom:0}),/positive/);
  assert.throws(()=>workshop.scanSize({...defaults,width:2000,height:2000,interval:.1}),/Increase/);
});
test('gray, gamma, brightness, contrast and threshold have independent effects',()=>{
  const samples=[0,64,128,255],dimensions={width:4,height:1};
  const gray=map(samples,dimensions).values;
  assert.equal(gray[0],1);assert.ok(gray[1]>gray[2]);assert.ok(gray[3]<1e-8);
  for (const adjustment of [{gamma:2},{brightness:25},{contrast:-100}])
    assert.ok(map(samples,{...dimensions,...adjustment}).values[1]<gray[1]);
  assert.deepEqual([...map(samples,{...dimensions,style:'threshold',threshold:30}).values],[1,1,0,0]);
});
test('alpha and artwork hole stay at S0 under inversion and dithering; odd scan row reversal',()=>{
  const outer=[[2,2],[6,2],[6,4],[2,4],[2,2]], hole=[[3,2],[4,2],[4,4],[3,4],[3,2]];
  const samples=Array(8).fill(255);samples[2]=[255,255,255,0];
  const options={...defaults,mask:'selection',contours:[outer,hole],invert:true,style:'dither'};
  const result=map(samples,options);
  assert.deepEqual([...result.values],[1,0,0,1,1,0,1,1]);
  const {paths,powers}=workshop.scanPaths(result,options);
  assert.deepEqual(powers,[[0,0,1,0,0,1,0],[0,0,1,1,0,1,0]]);
  assert.deepEqual(paths[1][0],[6.5,2.5]);assert.deepEqual(paths[1].at(-1),[1.5,2.5]);
  const job=cam.generate({...base,paths,path_powers:powers.map(row=>row.map(v=>v*1000))});
  assert.deepEqual(job.segments.filter(s=>s[5]===1000).length,5);
});
test('ellipse mask and zero overscan have no extra powered move',()=>{
  const opts={...defaults,width:4,height:4,mask:'ellipse',overscan:0,invert:true,style:'dither'};
  const result=map(Array(16).fill(255),opts);
  assert.equal(result.values[0],0);assert.equal(result.values[5],1);
  const {paths,powers}=workshop.scanPaths(result,opts);
  assert.equal(paths[0].length,5);assert.equal(powers[0][0],0);
});
test('saved grid operations retain individual speeds and S values through generation',()=>{
  const cells=workshop.materialGrid(grid);
  assert.equal(cells.length,6);
  const loaded=JSON.parse(JSON.stringify(cells));
  const project=layers.assemble({...base,feed:999,power:10},loaded,layers.defaults());
  assert.deepEqual(project.operations.map(o=>[o.feed,o.power]),[[300,200],[300,400],[300,600],[700,200],[700,400],[700,600]]);
  const job=cam.generate(project);
  assert.equal(job.operations,6);assert.ok(job.lines.some(line=>/F700\.0 S600/.test(line)));
  assert.deepEqual(workshop.materialGrid({...grid,pattern:'hatch'})[0].paths[1],[[5,2.75],[1,2.75]]);
  assert.throws(()=>workshop.materialGrid({...grid,x:199}),/work area/);
  assert.throws(()=>workshop.materialGrid({...grid,rows:11}),/integer/);
});
