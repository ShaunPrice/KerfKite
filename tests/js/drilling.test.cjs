const test=require('node:test'), assert=require('node:assert/strict');
const {generate,simulate}=require('../../studio/static/offline-cam.js');
const circle=Array.from({length:33},(_,i)=>[20+2*Math.cos(i*Math.PI/16),20+2*Math.sin(i*Math.PI/16)]);
const sample=overrides=>({mode:'router',operation:'drill',width:100,height:100,safe:3,depth:2.5,
  tool_diameter:4,power:9000,feed:100,plunge:80,paths:[circle,[[40,30],[40,30]]],...overrides});
test('single and peck drilling retract before traverses and post controlled Z feeds',()=>{
  for (const [peck,z] of [[0,[-2.5]],[1,[-1,-2,-2.5]]]){
    const job=generate(sample({drill_peck:peck}));assert.deepEqual(job.segments.map(s=>s[4]),z.concat(z));
    assert.equal(job.lines.at(-1),'M5');assert.equal(job.drill_holes.length,2);
    for(let i=0;i<job.lines.length;i++) if(job.lines[i].startsWith('G1 ')){
      assert.match(job.lines[i],/^G1 Z-.* F80\.0$/);assert.equal(job.lines[i+1],'G0 Z3.000');
    }
  }
});
test('drill tip stock contact on vertical plunges matches its included angle',()=>{
  const job=generate(sample({drill_peck:1}));
  const stock=simulate({segments:job.segments,resolution:17,x:18,y:18,width:4,height:4,tool_shape:'drill',v_angle:118,tool_diameter:4});
  assert.ok(Math.abs(stock.heights[8][8]+2.5)<1e-8);
  assert.ok(Math.abs(stock.heights[8][12]+2.5-1/Math.tan(59*Math.PI/180))<1e-8);
  assert.equal(stock.heights[0][0],0);
});
test('invalid tool, envelope, geometry or pecks are rejected',()=>{
  for(const p of [{paths:[[[10,10],[20,20]]]},{tool_diameter:5},{mode:'knife'},{tool_shape:'ball'},{drill_peck:.001},
    {paths:[[[1,1],[1,1]]]},{tool_shape:'drill',operation:'line'},{path_depths:[[0,0]]}]) assert.throws(()=>generate(sample(p)));
});
