// The offline generator's pen programs: the same as the core's (tests/test_pen.py holds the same lines).
const test=require('node:test');
const assert=require('node:assert/strict');
const path=require('node:path');
const root=path.join(__dirname,'../../studio/static');
require(path.join(root,'protocols.js'));
for(const script of ['drilling.js','router-entry.js'])try{require(path.join(root,script));}catch{}
const offline=require(path.join(root,'offline-cam.js'));
const PATHS=[[[10,10],[50,10],[50,40]],[[20,20],[30,30]]];
const project=changes=>({mode:'pen',protocol:'grbl',width:200,height:150,feed:1500,plunge:600,power:1000,safe:3,depth:1,paths:PATHS,...changes});

test('a Z pen presses, draws and lifts with the spindle off',()=>{
  const job=offline.generate(project());
  assert.equal(job.mode,'pen');
  assert.deepEqual(job.lines,['G21','G90','G17','G94','G54','M5','G0 Z3.000',
    'G0 Z3.000','G0 X10.000 Y10.000','G1 Z-1.000 F600.0','G1 X50.000 Y10.000 F1500.0','G1 X50.000 Y40.000 F1500.0','G0 Z3.000',
    'G0 Z3.000','G0 X20.000 Y20.000','G1 Z-1.000 F600.0','G1 X30.000 Y30.000 F1500.0','G0 Z3.000','M5']);
  assert.ok(job.warnings.some(w=>w.includes('spindle stays off')));
});
test('a servo pen is worked from the output and never moves Z',()=>{
  const job=offline.generate(project({pen_lift:'servo'}));
  assert.deepEqual(job.lines,['G21','G90','G17','G94','G54','M5','M5','G4 P0.200',
    'G0 X10.000 Y10.000','M3 S1000','G4 P0.200','G1 X50.000 Y10.000 F1500.0','G1 X50.000 Y40.000 F1500.0','M5','G4 P0.200',
    'G0 X20.000 Y20.000','M3 S1000','G4 P0.200','G1 X30.000 Y30.000 F1500.0','M5','G4 P0.200','M5']);
  assert.equal(job.segments.length,3);
  const held=offline.generate(project({pen_lift:'servo',pen_up_power:300,pen_dwell:.35})).lines;
  assert.deepEqual(held.slice(6,8),['M3 S300','G4 P0.350']);
  assert.deepEqual(held.slice(-2),['M3 S300','G4 P0.350']);
  assert.ok(!held.slice(6).includes('M5'));
  assert.ok(offline.generate(project({pen_lift:'servo',protocol:'marlin'})).lines.includes('G4 P200'));
});
test('a broad nib and repeated passes match the core',()=>{
  const job=offline.generate(project({paths:[[[20,20],[60,20]]],pen_nib:1.2,pen_nib_angle:90,pen_tip:.4}));
  const starts=job.lines.filter(l=>l.startsWith('G0 X'));
  assert.deepEqual(starts,['G0 X20.000 Y19.400','G0 X60.000 Y19.800','G0 X20.000 Y20.200','G0 X60.000 Y20.600']);
  assert.equal(offline.generate(project({passes:2})).lines.filter(l=>l==='G1 Z-1.000 F600.0').length,4);
  assert.throws(()=>offline.generate(project({paths:[[[.2,10],[50,10]]],pen_nib:2,pen_nib_angle:0})),/outside the work area/);
  assert.throws(()=>offline.generate(project({pen_lift:'magnet'})),/Pen lift must be z or servo/);
  assert.throws(()=>offline.generate(project({mode:'plasma'})),/Choose router, laser or knife/);
});
