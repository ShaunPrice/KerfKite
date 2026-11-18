const test=require('node:test'), assert=require('node:assert/strict');
const {generate}=require('../../studio/static/offline-cam.js');
const sample=options=>({mode:'router',width:90,height:90,safe:3,depth:2,step:1,stock_thickness:4,tool_diameter:4,
  power:9000,feed:400,plunge:90,paths:[[[10,10],[40,10],[40,40],[10,40],[10,10]]],router_entry:'ramp',...options});
test('offline contour ramp posts descending XYZ and a complete finishing lap at each depth',()=>{
  const job=generate(sample());
  assert.ok(job.lines.includes('G1 X40.000 Y10.000 Z-0.250 F90.0'));
  for(const z of [-1,-2])assert.equal(job.segments.filter(s=>s[4]===z&&s[6]===z).length,4);
});
test('offline refuses unsupported entry geometry and pocket helix explicitly',()=>{
  for(const options of [{paths:[[[10,10],[20,10]]]},{paths:[[[10,10],[20,20],[20,10],[10,20],[10,10]]]},
    {tool_shape:'ball'},{stock_thickness:1},{router_entry:'helix',operation:'pocket'},{operation:'drill'}])
    assert.throws(()=>generate(sample(options)));
});
