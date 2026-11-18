const test=require('node:test');
const assert=require('node:assert/strict');
const {generate}=require('../../studio/static/offline-cam.js');
const make=(args={})=>({mode:'laser',width:100,height:100,power:800,feed:600,paths:[[[10,10],[14,10],[14,16],[26,16]]],...args});
const offLength=job=>job.segments.filter(s=>s[5]===0).reduce((d,s)=>d+Math.hypot(s[2]-s[0],s[3]-s[1]),0);

test('perforation continues across a corner and repeats in the same place on each pass',()=>{
  const job=generate(make({laser_pattern:'perforate',laser_dash:3,laser_gap:2,lead_in:2,lead_out:3,passes:2}));
  assert.equal(offLength(job),16);
  for(const expected of ['G1 X14.000 Y10.000 F600.0 S0','G1 X14.000 Y11.000 F600.0 S0','G1 X29.000 Y16.000 F600.0 S800'])
    assert.ok(job.lines.includes(expected));
});
test('holding bridges override power ramps on closed contours',()=>{
  const job=generate(make({paths:[[[20,20],[40,20],[40,40],[20,40],[20,20]]],laser_pattern:'bridges',
    laser_gap:2,laser_bridge_count:4,ramp_in:15,start_power:20}));
  assert.equal(offLength(job),8);
  assert.ok(job.segments.some(s=>s[5]>0&&s[5]<800));
});
test('quarter-circle leads use programmed arc length and travel side',()=>{
  for(const [shape,side] of [['arc-left',1],['arc-right',-1]]){
    const job=generate(make({paths:[[[30,30],[60,30]]],lead_shape:shape,lead_in:5*Math.PI,lead_out:5*Math.PI}));
    const moves=job.segments;
    assert.ok(Math.abs(moves[0][0]-20)<1e-8&&Math.abs(moves[0][1]-(30+10*side))<1e-8);
    assert.ok(Math.abs(moves.at(-1)[2]-70)<1e-8&&Math.abs(moves.at(-1)[3]-(30+10*side))<1e-8);
    assert.ok(Math.abs(job.cut_length-(30+10*Math.PI))<.04);
  }
  assert.throws(()=>generate(make({lead_shape:'arc-right',lead_in:80})),/outside/);
});
test('invalid and oversized patterns fail before output',()=>{
  for(const args of [{laser_pattern:'bridges'},{laser_pattern:'bridges',laser_bridge_count:1.5},
    {laser_pattern:'perforate',laser_gap:0},{laser_pattern:'perforate',path_powers:[[0,0,0,0]]},
    {laser_pattern:'perforate',laser_gap:.01,laser_dash:.01,width:2000,height:2000,paths:[[[0,0],[2000,2000]]]}])
    assert.throws(()=>generate(make(args)));
});
