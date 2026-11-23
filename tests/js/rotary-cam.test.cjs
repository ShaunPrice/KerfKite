const test=require('node:test'),assert=require('node:assert/strict');
const {wrap}=require('../../studio/static/rotary-cam.js');
const {generate}=require('../../studio/static/offline-cam.js');
function source(end=Math.PI*10){return {mode:'router',lines:['G21','G90','G17','G94','G54','M5','G0 Z5','M3 S1000','G4 P2','G0 X0 Y0','G1 Z-1 F100',`G1 X${end.toFixed(3)} Y7 F600`,'G0 Z5','M5']};}
const options={diameter:10,wrap_axis:'X',postprocessor:'linuxcnc'};
test('full revolution snaps source rounding, maps axial X and restores feed mode',()=>{const r=wrap(source(),options),cut=r.lines.find(l=>l.startsWith('G1 A'));assert.match(cut,/A360\.000000 X7\.000000/);assert.ok(!cut.includes('Y'));assert.ok(r.lines.includes('G93'));assert.equal(r.lines.at(-1),'G94 F600.000000');assert.equal(r.rotary.export_only,true);});
test('continuous reverse angles exceed a full turn and coordinated feed includes Z',()=>{const r=wrap(source(Math.PI*20),{...options,direction:-1,angle_offset:15});assert.equal(r.rotary.angle_bounds[0],-705);const s=source();s.lines[11]='G1 X3 Y4 Z-2 F600';const t=wrap(s,{...options,wrap_axis:'Y'}),line=t.lines.find(l=>l.startsWith('G1 X3'));assert.match(line,/A45\.836624 Z-2\.000000/);assert.ok(Math.abs(Number(line.split(' F')[1])-600/Math.sqrt(26))<1e-8);});
test('reject unsupported source, numeric values and missing final spindle off',()=>{for(const command of ['G91','G2 X1 Y2 I0 J1','M4 S100','G1 X1 Y1 S20 F100','G1 X1 Y1','G1 X1 X2 F100','G1 Z-5 F100','G1 X1 Y1 F0','G1 Xnan Y1 F100','G1 X1 Y1 F100 ;comment','G20']){const s=source();s.lines[11]=command;assert.throws(()=>wrap(s,options),command);}for(const changes of [{diameter:NaN},{diameter:true},{direction:0},{postprocessor:'grbl'},{wrap_axis:'Z'}])assert.throws(()=>wrap(source(),{...options,...changes}));const s=source();s.lines.pop();assert.throws(()=>wrap(s,options));assert.throws(()=>wrap({...source(),mode:'laser'},options));});
test('offline contour CAM feeds rotary export without a server',()=>{const p={mode:'router',protocol:'grbl',width:100,height:100,feed:600,power:1000,depth:1,step:1,plunge:100,safe:5,paths:[[[0,0],[31.416,7]]]};const r=wrap(generate(p),options);assert.ok(r.lines.some(l=>l.includes('A360.000000')));});
// The C++ core's wrapping of given lines, from kerfkite-cam: KERFKITE_CAM, or the build in build/native. Without
// one the comparisons are skipped, saying so.
const CAM=(()=>{const path=require('node:path'),fs=require('node:fs'),root=path.resolve(__dirname,'../..'),exe=process.platform==='win32'?'.exe':'';return process.env.KERFKITE_CAM||['build/native','build/native/Release'].map(d=>path.join(root,d,'kerfkite-cam'+exe)).find(f=>fs.existsSync(f));})();
const withCore={skip:CAM?false:'kerfkite-cam is not built: build the native programs (native/README.md) or set KERFKITE_CAM'};
function core(job,options){const {spawnSync}=require('node:child_process');const result=spawnSync(CAM,['rotary-wrap','-'],{input:JSON.stringify({job,options}),encoding:'utf8'});assert.equal(result.status,0,result.stderr);return JSON.parse(result.stdout);}
test('browser implementation agrees with the C++ core across axes and directions',withCore,()=>{const fixtures=[];for(const axis of ['X','Y'])for(const direction of [-1,1])fixtures.push({job:source(),options:{...options,wrap_axis:axis,direction}});fixtures.forEach(f=>assert.deepEqual(wrap(f.job,f.options).lines,core(f.job,f.options).lines));});
test('laser moves preserve individual powers, with reverse second-turn wrapping and fixture limits',()=>{
  const laser={mode:'laser',lines:['G21','G90','G17','G94','G54','M5','G0 X0 Y0','M4 S0','G1 X3 Y31.416 F600 S125','G1 X5 Y62.832 F600 S750','M5']};
  const opts={diameter:10,wrap_axis:'Y',postprocessor:'grblhal',direction:-1,output_axis:'B',fixture:{axial_min:0,axial_max:5,min_radius:5}};
  const r=wrap(laser,opts);
  assert.deepEqual(r.rotary.angle_bounds,[-720,0]);assert.deepEqual(r.rotary.axial_bounds,[0,5]);assert.deepEqual(r.rotary.radial_bounds,[0,0]);
  assert.match(r.lines.join('\n'),/G1 Y3\.000000 B-360\.000000 F[\d.]+ S125\nG1 Y5\.000000 B-720\.000000 F[\d.]+ S750/);
  assert.throws(()=>wrap(laser,{...opts,fixture:{...opts.fixture,axial_max:4}}));
  assert.throws(()=>wrap({...laser,lines:laser.lines.toSpliced(-1,0,'G0 X4')},opts));
  assert.throws(()=>wrap(laser,{...opts,postprocessor:'xpro-v5'}));
});
test('the C++ core and portable post agree on laser power, target words and radius-aware helical feeds',withCore,()=>{
  const p={mode:'laser',width:100,height:100,feed:350,power:500,paths:[[[0,4],[20,6],[33,8]]],path_powers:[[100,150,350]]};
  const r={mode:'router',width:100,height:100,feed:300,power:800,depth:2,step:2,plunge:100,safe:2,paths:[[[0,4],[20,6],[33,8]]],path_depths:[[-.2,-1.3,-.5]]};
  const fixtures=[];
  for(const design of [p,r])for(const postprocessor of ['grblhal','xpro-v5'])for(const output_axis of (postprocessor==='xpro-v5'?['A']:['A','B','C'])){
    fixtures.push({job:generate({...design,protocol:'grbl'}),options:{diameter:14,wrap_axis:'X',postprocessor,output_axis,feed_reference:'cut',fixture:{axial_min:0,axial_max:10,min_radius:4}}});
  }
fixtures.forEach(({job:input,options:opts})=>{const job=core(input,opts),js=wrap(input,opts);assert.deepEqual(js.lines,job.lines);assert.deepEqual(js.rotary.radial_bounds,job.rotary.radial_bounds);assert.ok(Math.abs(js.rotary.feed_minutes-job.rotary.feed_minutes)<1e-10);});
});
