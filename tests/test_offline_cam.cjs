const {test}=require('node:test');
const assert=require('node:assert/strict');
const {generate}=require('../studio/static/offline-cam.js');
const sample=(changes={})=>({mode:'router',operation:'line',width:100,height:100,feed:600,power:1000,safe:5,depth:1.2,step:.5,plunge:100,passes:1,paths:[[[10,10],[20,10],[20,20]]],...changes});

test('router centreline retracts and steps down to exact depth',()=>{
  const result=generate(sample());
  assert.deepEqual(result.lines.filter(line=>line.startsWith('G1 Z')),['G1 Z-0.500 F100.0','G1 Z-1.000 F100.0','G1 Z-1.200 F100.0']);
  result.lines.forEach((line,i)=>{if(line.startsWith('G0 X')) assert.equal(result.lines[i-1],'G0 Z5.000');});
  assert.deepEqual(result.lines.slice(-2),['G0 Z5.000','M5']);
  assert.equal(result.cut_length,60);assert.equal(result.cut_minutes,.1);
  assert.deepEqual(result.bounds,[10,10,20,20]);
});

test('laser travel is off with no Z and repeat passes',()=>{
  const result=generate(sample({mode:'laser',passes:2}));
  assert.equal(result.lines.filter(line=>line==='M4 S1000').length,2);
  assert.ok(result.lines.every(line=>!line.includes('Z')));
  result.lines.forEach((line,i)=>{if(line.startsWith('G0 X')) assert.equal(result.lines[i-1],'M5');});
  assert.equal(result.cut_length,40);
});

test('laser tangent leads and midpoint ramps match expected envelope',()=>{
  const result=generate(sample({mode:'laser',paths:[[[10,10],[30,10]]],lead_in:2,lead_out:3,ramp_in:4,ramp_out:5,start_power:0,end_power:10}));
  assert.deepEqual(result.bounds,[8,10,33,10]);assert.equal(result.cut_length,25);
  assert.ok(result.lines.includes('M4 S0'));
  assert.ok(result.lines.includes('G1 X8.500 Y10.000 F600.0 S62'));
  const powers=result.segments.map(s=>s[5]);
  assert.ok(powers[0]<100);assert.equal(Math.max(...powers),1000);assert.ok(powers.at(-1)<200);
  assert.throws(()=>generate(sample({mode:'laser',lead_in:20})),/outside/);
  assert.throws(()=>generate(sample({mode:'laser',ramp_in:15,ramp_out:15})),/overlap/);
});

test('variable-depth contours apply step limits and conservative Z feed',()=>{
  const result=generate(sample({path_depths:[[-.1,-1,-2]]}));
  assert.equal(result.segments.length,8);
  assert.deepEqual(result.segments.map(s=>s[4]),[-.5,-.5,-1,-1,-1,-1.5,-1,-2]);
  assert.ok(result.lines.filter(line=>line.startsWith('G1 X')).every(line=>line.includes(' F100.0 Z')));
  assert.ok(result.lines.filter(line=>line.startsWith('G1 Z')).every(line=>line==='G1 Z-0.100 F100.0'));
});

test('knife emits compensated swivel contours without motor commands',()=>{
  const result=generate(sample({mode:'knife',depth:.1}));
  assert.ok(result.prepared_paths[0].length>10);
  assert.equal(result.prepared_paths[0][0][0],9.25);
  assert.ok(result.lines.every(line=>!line.startsWith('M3')&&!line.startsWith('M4')));
  assert.throws(()=>generate(sample({mode:'knife',paths:[[[0,0],[1,0]]]})),/Compensated X/);
});

test('Marlin router postprocessor uses millisecond dwell only for router',()=>{
  const result=generate(sample({protocol:'marlin'}));
  assert.ok(result.lines.includes('G4 P2000'));
  assert.ok(result.lines.every(line=>!['G17','G94','G54'].includes(line)));
  assert.throws(()=>generate(sample({mode:'laser',protocol:'marlin'})),/dynamic laser/);
});

test('invalid or unsupported requests fail explicitly without fallback',()=>{
  for(const changes of [{operation:'pocket'},{operation:'outside'},{operation:'fill'},{protocol:'unsupported'},{mode:'unknown'},
    {feed:NaN},{depth:true},{width:null},{passes:1.5},{step:0},{paths:[[[0,0],[101,0]]]},
    {paths:[[[0,0],[Infinity,0]]]},{paths:[[[0,0,0],[1,1]]]},{path_depths:[[-1]]},
    {path_depths:[[0,0,0]]},{mode:'laser',path_depths:[[-1,-1,-1]]},
    {mode:'laser',paths:[[[1,1],[1,1]]]},{mode:'knife',path_depths:[[-1,-1,-1]]},
    {depth:50,step:.01,paths:[Array.from({length:100},(_,i)=>[i,0])]},
    {paths:[Array.from({length:100001},()=>[1,1])]}]) {
    assert.throws(()=>generate(sample(changes)),undefined,JSON.stringify(changes).slice(0,100));
  }
  assert.throws(()=>generate(null));
});

test('input paths remain unchanged and duplicate laser points are accepted',()=>{
  const p=sample({mode:'laser',paths:[[[10,10],[10,10],[20,10],[20,10]]],lead_in:2});
  const copy=JSON.stringify(p);assert.equal(generate(p).cut_length,12);assert.equal(JSON.stringify(p),copy);
});

test('ordered operations preserve per-object feed power and safe boundaries',()=>{
  const p=sample({mode:'laser',operations:[
    {feed:300,power:500,paths:[[[10,10],[20,10]]]},
    {feed:900,power:1000,paths:[[[30,20],[40,20]]],path_powers:[[0,250]]}
  ]});
  const result=generate(p);
  assert.equal(result.operations,2);assert.equal(result.cut_length,20);
  assert.deepEqual(result.bounds,[10,10,40,20]);
  assert.ok(result.lines.includes('G1 X20.000 Y10.000 F300.0 S500'));
  assert.ok(result.lines.includes('G1 X40.000 Y20.000 F900.0 S250'));
  assert.equal(result.lines.filter(line=>line==='G21').length,2);
  result.lines.forEach((line,i)=>{if(line==='G21'&&i) assert.equal(result.lines[i-1],'M5');});
  assert.equal(result.warnings.length,new Set(result.warnings).size);
});

test('pixel powers retain zero-power moves and endpoint segment powers',()=>{
  const result=generate(sample({mode:'laser',path_powers:[[0,200,0]]}));
  assert.ok(result.lines.includes('M4 S0'));
  assert.ok(result.lines.includes('G1 X20.000 Y10.000 F600.0 S200'));
  assert.ok(result.lines.includes('G1 X20.000 Y20.000 F600.0 S0'));
  assert.deepEqual(result.segments,[[10,10,20,10,0,200,0],[20,10,20,20,0,0,0]]);
});

test('ordered job boundaries and pixel powers reject ambiguous input',()=>{
  const invalid=[{operations:[]},{operations:Array(101).fill({})},{operations:[null]},
    {operations:[{operations:[]}]},{operations:[{mode:'laser'}]},
    {operations:[{protocol:'marlin'}]},{operations:[{width:200}]},
    {operations:[{operation:'pocket'}]},{operations:[{feed:300}]},
    {mode:'laser',path_powers:[]},{mode:'laser',path_powers:[[1]]},
    {mode:'laser',path_powers:[[0,1001,0]]},{mode:'laser',path_powers:[[0,-1,0]]},
    {mode:'laser',path_powers:[[0,NaN,0]]},{mode:'laser',path_powers:[[0,1,0]],ramp_in:1},
    {path_powers:[[0,1,0]]}];
  for(const changes of invalid) assert.throws(()=>generate(sample(changes)));
});

test('offline rejects holding tabs rather than cutting through them',()=>{assert.throws(()=>generate(sample({tabs:{width:5,height:1,count:3}})),/Holding tabs/);assert.throws(()=>generate(sample({operations:[{paths:[[ [1,1],[2,1],[1,1] ]],tabs:{width:5,height:1,count:3}}]})),/Holding tabs/);});
