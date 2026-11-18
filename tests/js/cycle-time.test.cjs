const {test}=require('node:test');
const assert=require('node:assert/strict');
const {timing,calibrated}=require('../../studio/static/cycle-time.js');
const setup={rapid:[1200,600,100],start:[0,0,0]};
test('coordinated XYZ rapids, 3D feed, modal feed and controller-specific dwell',()=>{
  const program=['G21','G90','G0 X60 Y60 Z5','G1 X63 Y64 F100','G1 Z0','G4 P2000','M5'];
  const t=timing(program,setup,'marlin');
  assert.equal(t.travel,.1);assert.equal(t.cut,.1);assert.equal(t.dwell,2/60);
  assert.ok(Math.abs(calibrated(t,.4+t.dwell)-2)<1e-12);
  assert.equal(timing(program,setup,'mach3','milliseconds').dwell,t.dwell);
  assert.throws(()=>timing(program,setup,'mach3',''),/dwell units/);
  assert.throws(()=>timing(['G20','G91','G1 X10 F20'],setup,'grbl'),/cannot interpret/);
  assert.throws(()=>timing(['G21','G90','G1 A360 F20'],setup,'grbl'),/Invalid program word/);
});
