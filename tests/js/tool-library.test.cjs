const test=require('node:test'),assert=require('node:assert/strict');
const lib=require('../../studio/static/tool-library.js');
const mill={name:'Quarter inch',shape:'flat',diameter:6.35,flutes:2};
const rigid={rigidity:'rigid',minRpm:8000,maxRpm:24000,maxFeed:10000};

test('the feed is spindle speed × flutes × chip load, and the speed comes from the cutting speed',()=>{
  const r=lib.recommend(mill,'softwood',rigid);
  // 400 m/min at a 6.35 mm edge is 20,051 rev/min, rounded to the hundred.
  assert.equal(r.rpm,20100);
  const chip=.016*6.35;  // a rigid machine takes the whole chip
  assert.ok(Math.abs(r.feed-20051*2*chip)<=5,`feed ${r.feed}`);
  assert.ok(Math.abs(r.chip_load-chip)<.002);
  assert.equal(r.depth_per_pass,3.175);
  assert.equal(r.stepover,2.54);
  assert.equal(r.plunge,Math.round(r.feed*.35/5)*5);
  assert.deepEqual(r.warnings,[]);
});
test('a lighter machine takes a thinner chip and a shallower cut',()=>{
  const heavy=lib.recommend(mill,'hardwood',rigid),light=lib.recommend(mill,'hardwood',{...rigid,rigidity:'light'});
  assert.equal(light.rpm,heavy.rpm);
  assert.ok(Math.abs(light.feed-heavy.feed/2)<=5);
  assert.ok(Math.abs(light.depth_per_pass-heavy.depth_per_pass/2)<.001);
});
test('the spindle stays inside the range of the machine',()=>{
  const small={name:'Eighth',shape:'flat',diameter:3.175,flutes:2};
  assert.equal(lib.recommend(small,'softwood',rigid).rpm,24000);  // 40,100 wanted
  const wide={name:'Surfacer',shape:'surfacing',diameter:25.4,flutes:2};
  assert.equal(lib.recommend(wide,'mdf',rigid).rpm,8000);  // 5,000 wanted
});
test('when the machine cannot feed fast enough the spindle slows to keep the chip',()=>{
  const slow={...rigid,maxFeed:2500};
  const r=lib.recommend(mill,'softwood',slow),free=lib.recommend(mill,'softwood',rigid);
  assert.equal(r.feed,2500);
  assert.ok(r.rpm<free.rpm&&r.rpm>=slow.minRpm);
  assert.ok(Math.abs(r.chip_load-free.chip_load)<.004,'the chip is kept');
  assert.deepEqual(r.warnings,[]);
  // Below the lowest speed the chip thins, and the answer says so.
  const stuck=lib.recommend(mill,'softwood',{...rigid,maxFeed:400});
  assert.equal(stuck.rpm,8000);assert.equal(stuck.feed,400);
  assert.ok(stuck.chip_load<free.chip_load/2);
  assert.match(stuck.warnings.join(' '),/may rub/);
});
test('more flutes feed faster at the same chip',()=>{
  const one=lib.recommend({...mill,flutes:1},'acrylic',rigid),two=lib.recommend(mill,'acrylic',rigid);
  assert.ok(Math.abs(two.feed-2*one.feed)<=10);
  assert.ok(Math.abs(two.chip_load-one.chip_load)<.002);
});
test('a finishing pass is lighter than a profile, and a pocket steps over 40 % of a flat cutter',()=>{
  const profile=lib.recommend(mill,'plywood',rigid),finish=lib.recommend(mill,'plywood',rigid,{cut:'finish'});
  assert.ok(finish.feed<profile.feed&&finish.depth_per_pass<profile.depth_per_pass&&finish.stepover<profile.stepover);
  assert.equal(lib.recommend(mill,'plywood',rigid,{cut:'pocket'}).stepover,2.54);
  assert.equal(lib.recommend({...mill,shape:'ball'},'plywood',rigid,{cut:'finish'}).stepover,.635);
});
test('a V bit is rated on the width it cuts at the depth',()=>{
  const v={name:'Sixty',shape:'v',diameter:12.7,flutes:2,angle:60};
  const shallow=lib.recommend(v,'hardwood',rigid,{depth:1}),deep=lib.recommend(v,'hardwood',rigid,{depth:6});
  assert.ok(Math.abs(shallow.working_diameter-2*Math.tan(Math.PI/6))<.001);
  assert.ok(deep.working_diameter>shallow.working_diameter&&deep.working_diameter<=12.7);
  assert.ok(deep.feed>shallow.feed||deep.rpm<shallow.rpm);
  assert.equal(lib.fields(shallow).v_angle,60);
});
test('a drill plunges at its feed and pecks its own diameter',()=>{
  const r=lib.recommend({name:'Three',shape:'drill',diameter:3,flutes:2,angle:118},'aluminium',rigid);
  assert.equal(r.plunge,r.feed);
  assert.equal(r.depth_per_pass,3);
  assert.deepEqual(lib.fields(r),{tool_diameter:3,feed:r.feed,plunge:r.feed,power:r.rpm,step:3,stepover:3,drill_point_angle:118,drill_peck:3});
  assert.match(r.warnings.join(' '),/Metal needs a rigid machine/);
});
test('the fields are the CAM settings of the editor',()=>{
  const r=lib.recommend(mill,'mdf',rigid);
  assert.deepEqual(lib.fields(r),{tool_diameter:6.35,feed:r.feed,plunge:r.plunge,power:r.rpm,step:r.depth_per_pass,stepover:r.stepover});
});
test('every starter tool gives usable settings in every material on every machine',()=>{
  const start=lib.starter();
  assert.equal(start.format,'kerfkite-tools/1');
  for(const tool of start.tools)for(const material of lib.MATERIALS)for(const rigidity of Object.keys(lib.RIGIDITY))for(const cut of Object.keys(lib.CUTS)){
    const r=lib.recommend(tool,material.id,{...start.machine,rigidity},{cut});
    assert.ok(r.rpm>=start.machine.minRpm&&r.rpm<=start.machine.maxRpm,`${tool.name} ${material.id} rpm ${r.rpm}`);
    assert.ok(r.feed>=1&&r.feed<=start.machine.maxFeed,`${tool.name} ${material.id} feed ${r.feed}`);
    assert.ok(r.plunge>=1&&r.plunge<=r.feed);
    assert.ok(r.depth_per_pass>0&&r.stepover>0&&r.chip_load>0);
  }
});
test('tools, machines and libraries are validated',()=>{
  assert.deepEqual(lib.tool({name:' Bit ',shape:'flat',diameter:'3.175',flutes:2,number:'4',notes:' spare '}),{name:'Bit',shape:'flat',diameter:3.175,flutes:2,number:4,notes:'spare'});
  assert.equal(lib.tool({name:'V',shape:'v',diameter:6}).angle,60);
  for(const bad of [null,[],{name:'',shape:'flat',diameter:3},{name:'x',shape:'cone',diameter:3},{name:'x',shape:'flat',diameter:0},{name:'x',shape:'flat',diameter:3,flutes:1.5},{name:'x',shape:'v',diameter:3,angle:5},{name:'x',shape:'flat',diameter:3,number:1000},{name:'x\ny',shape:'flat',diameter:3},{name:'x',shape:'flat',diameter:NaN}])
    assert.throws(()=>lib.tool(bad),Error,JSON.stringify(bad));
  assert.throws(()=>lib.machine({minRpm:20000,maxRpm:10000}),/lowest spindle speed is above/);
  assert.throws(()=>lib.machine({rigidity:'wobbly'}),/light, medium or rigid/);
  assert.throws(()=>lib.recommend(mill,'granite',rigid),/Choose a material/);
  assert.throws(()=>lib.recommend(mill,'mdf',rigid,{cut:'plunge'}),/profile, pocket or finishing/);
  assert.throws(()=>lib.library({format:'other',tools:[]}),/not a KerfKite tool library/);
  assert.throws(()=>lib.library({format:lib.FORMAT,tools:[mill,{...mill,name:'QUARTER INCH'}]}),/same name/);
  assert.throws(()=>lib.library({format:lib.FORMAT,tools:Array.from({length:201},(_,i)=>({...mill,name:'t'+i}))}),/at most 200/);
  const round=lib.library(JSON.parse(JSON.stringify(lib.starter())));
  assert.deepEqual(round,lib.starter());
});
