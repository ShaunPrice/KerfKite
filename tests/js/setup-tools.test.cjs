const test=require('node:test'),assert=require('node:assert/strict');
const tools=require('../../studio/static/setup-tools.js');

test('steps per mm are corrected by commanded over measured',()=>{
  assert.deepEqual(tools.calibrate(80,100,100),{steps:80,change:0});
  // Told to move 100 mm, the axis moved 99.2: it needs more steps for each millimetre.
  const r=tools.calibrate(80,100,99.2);
  assert.ok(Math.abs(r.steps-80*100/99.2)<.0006);assert.ok(r.change>0);
  assert.ok(tools.calibrate(400,50,50.4).steps<400);
  // Applying the correction makes the same command come out right: steps × measured is what the move took.
  assert.ok(Math.abs(r.steps*99.2/80-100)<.001);
});
test('a correction too large to be a calibration is refused',()=>{
  assert.throws(()=>tools.calibrate(80,100,50),/would change the steps per mm by 100\.0%/);
  assert.throws(()=>tools.calibrate(80,100,125.1),/Check the measurement/);
  assert.throws(()=>tools.calibrate(0,100,100),/Current steps per mm/);
  assert.throws(()=>tools.calibrate(80,0,100),/Commanded distance/);
  assert.throws(()=>tools.calibrate(80,100,'far'),/Measured distance/);
});
test('a surfacing pass covers the whole area and stays inside it',()=>{
  for(const [w,h,d,over] of [[300,200,25.4,15],[200,300,25.4,15],[100,100,20,20],[90,40,22,7],[25.4,25.4,25.4,5]]){
    const {path,passes,pitch}=tools.surfacing(w,h,d,over),r=d/2;
    assert.ok(pitch<=over+1e-9,`pitch ${pitch} within stepover ${over}`);
    for(const [x,y] of path)assert.ok(x>=r-1e-9&&x<=w-r+1e-9&&y>=r-1e-9&&y<=h-r+1e-9,'the cutter stays inside the area');
    for(let i=0;i<path.length-1;i++)assert.ok(Math.abs(path[i][0]-path[i+1][0])<1e-9||Math.abs(path[i][1]-path[i+1][1])<1e-9,'moves are along an axis');
    // Every point of the area, apart from the rounded corners, is within a radius of the path.
    const near=(px,py)=>path.slice(0,-1).some((a,i)=>{const b=path[i+1],dx=b[0]-a[0],dy=b[1]-a[1],L=dx*dx+dy*dy,t=L?Math.max(0,Math.min(1,((px-a[0])*dx+(py-a[1])*dy)/L)):0;return Math.hypot(px-a[0]-t*dx,py-a[1]-t*dy)<=r+1e-9;});
    for(let x=r;x<=w-r;x+=Math.max(1,w/37))for(let y=0;y<=h;y+=Math.max(1,h/41))assert.ok(near(x,y),`(${x},${y}) is cut in ${w}×${h}`);
    for(let y=r;y<=h-r;y+=Math.max(1,h/37))for(let x=0;x<=w;x+=Math.max(1,w/41))assert.ok(near(x,y),`(${x},${y}) is cut in ${w}×${h}`);
    assert.ok(passes>=1);
  }
  // Passes run along the longer side.
  const wide=tools.surfacing(300,200,25.4,15).path,tall=tools.surfacing(200,300,25.4,15).path;
  assert.equal(wide[0][1],wide[1][1]);assert.equal(tall[0][0],tall[1][0]);
});
test('surfacing requests are checked',()=>{
  assert.throws(()=>tools.surfacing(300,200,25.4,30),/must not be more than the cutter diameter/);
  assert.throws(()=>tools.surfacing(20,200,25.4,10),/smaller than the cutter/);
  assert.throws(()=>tools.surfacing(300,200,0,10),/Cutter diameter/);
  assert.throws(()=>tools.surfacing(300,200,25.4,0),/Stepover/);
});
test('macros hold plain G-code words, as the server accepts them',()=>{
  assert.deepEqual(tools.macroLines('g90 ; absolute\n\n  G0 x12.5 Y-3  \r\nM5'),['G90','G0 X12.5 Y-3','M5']);
  for(const bad of ['$100=5','$H','G1 X#1','G0 X1 (note)','%','!'])assert.throws(()=>tools.macroLines(bad),/plain G-code words only/,bad);
  assert.throws(()=>tools.macroLines('12 G0'),/starts with a G-code word/);
  assert.throws(()=>tools.macroLines('; nothing'),/no G-code in it/);
  assert.throws(()=>tools.macroLines('G0 X'+'1'.repeat(80)),/at most 78 characters/);
  assert.throws(()=>tools.macroLines(Array(101).fill('G4 P0').join('\n')),/1 to 100 lines/);
  assert.throws(()=>tools.macroLines(5),/A macro is text/);
});
test('a macro library is validated and the starters are sound',()=>{
  const start=tools.starterMacros();
  assert.equal(start.format,'kerfkite-macros/1');assert.equal(start.macros.length,4);
  for(const m of start.macros)assert.ok(tools.macroLines(m.text).length>=1);
  assert.deepEqual(tools.macroLibrary(JSON.parse(JSON.stringify(start))),start);
  assert.throws(()=>tools.macroLibrary({format:'x',macros:[]}),/not a KerfKite macro library/);
  assert.throws(()=>tools.macroLibrary({format:tools.MACRO_FORMAT,macros:[{name:'a',text:'M5'},{name:'A',text:'M5'}]}),/same name/);
  assert.throws(()=>tools.macro({name:'',text:'M5'}),/name of 1 to 60/);
  assert.throws(()=>tools.macro({name:'x',text:'$X'}),/plain G-code words only/);
});
test('the steps settings are the ones Grbl uses',()=>{
  assert.deepEqual(tools.STEPS,{X:'100',Y:'101',Z:'102'});
  assert.equal(tools.SETTING_NAMES[100],'X steps per mm');assert.equal(tools.SETTING_NAMES[32],'Laser mode');
});
