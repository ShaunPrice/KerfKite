const test=require('node:test'),assert=require('node:assert/strict');
const Pcb=require('../../studio/static/pcb-board.js');
const {generate}=require('../../studio/static/offline-cam.js');

const values=overrides=>({side:'top',margin:5,width:300,height:180,registered:null,kind:'v',tip:0.1,angle:30,depth:0.1,toolWidth:0.2,
  passes:1,overlapPercent:20,arcTolerance:0.005,direction:'climb',clip:false,units:'',zeros:'',integer:null,decimal:null,
  drillDepth:1.8,cutter:1.5,cutDepth:1.8,cutStep:0.6,peck:0,pointAngle:118,...overrides});
const square=(x,y,s)=>[[x,y],[x+s,y],[x+s,y+s],[x,y+s],[x,y]];
// A server answer in the shape of /api/pcb-import, small enough to read.
const answer=overrides=>({side:'top',offset:[-95,115],mirror_x:null,bounds:[5,5,45,35],warnings:[],
  copper:{tool_width:0.15359,islands:2,path_count:2,point_count:10,unreachable_count:1,unreachable:[{at:[20,20],gap:0.12,between:[[19.94,20],[20.06,20]],islands:[0,1]}],
    layer:{units:'mm',file_function:'Copper,L1,Top',counts:{flash:2,line:1,arc:0,region:0},bounds:[0,0,1,1]},
    operations:[{operation:'line',paths:[square(10,10,5)]},{operation:'line',paths:[square(20,10,5)]}],copper:[{shell:square(10.1,10.1,4.8),holes:[]}],
    warnings:['The cutter cannot pass between separate copper features in 1 place']},
  outline:{paths:[square(5,5,30),square(12,12,2)],bounds:[5,5,35,35],line_width:0.1,warnings:['The outline has 2 closed loops']},
  drill:{groups:[{tool_diameter:0.4,holes:2,tools:[1]},{tool_diameter:0.8,holes:1,tools:[2,4]}],
    operations:[{operation:'drill',tool_shape:'drill',tool_diameter:0.4,paths:[[[10,30],[10,30]],[[15.5,29.75],[15.5,29.75]]]},
      {operation:'drill',tool_shape:'drill',tool_diameter:0.8,paths:[[[25,25],[25,25]]]}],
    slots:[{tool:3,diameter:1,points:[[30,10],[34,10]]},{tool:3,diameter:1,points:[[17,6],[17,10]]}],bounds:[9.8,5.5,34.5,30.2],
    interpretation:{units:'mm',units_from:'header',integer:3,decimal:3,digits_from:'default',omitted_zeros:'leading',zeros_from:'header'},
    warnings:['State "integer" and "decimal" in the request if this is wrong']},...overrides});

test('the width a V-bit cuts is its tip plus the cone on both sides',()=>{
  assert.equal(Pcb.vBitWidth(0.1,30,0.1),0.15359);
  assert.equal(Pcb.vBitWidth(0.2,90,0.05),0.3);
  assert.equal(Pcb.vBitWidth(0,60,0.1),Math.round(0.2*Math.tan(Math.PI/6)*1e6)/1e6);
  for(const bad of [[0.1,30,0],[0.1,180,0.1],[-1,30,0.1],[NaN,30,0.1],['0.1',30,0.1]])assert.throws(()=>Pcb.vBitWidth(...bad));
});

test('the request states the tool, the placement and only the drill format that was entered',()=>{
  const files={copper:'G04*',outline:'G04*',drill:'M48'};
  const body=Pcb.request(files,values());
  assert.deepEqual(body,{copper:'G04*',outline:'G04*',drill:'M48',side:'top',width:300,height:180,margin:5,
    v_bit:{tip_width:0.1,angle:30,depth:0.1},passes:1,overlap:0.2,arc_tolerance:0.005});
  const stated=Pcb.request(files,values({kind:'width',toolWidth:0.25,passes:3,overlapPercent:35,direction:'conventional',clip:true,units:'inch',zeros:'trailing',integer:2,decimal:4}));
  assert.equal(stated.tool_width,0.25);assert.equal(stated.v_bit,undefined);
  assert.deepEqual([stated.passes,stated.overlap,stated.direction,stated.clip],[3,0.35,'conventional',true]);
  assert.deepEqual(stated.drill_format,{units:'inch',omitted_zeros:'trailing',integer:2,decimal:4});
  // Only the files that were chosen are sent, and a drill file alone needs no tool.
  assert.deepEqual(Pcb.request({copper:'',outline:null,drill:'M48'},values({depth:null,tip:null})),{drill:'M48',side:'top',width:300,height:180,margin:5});
  assert.throws(()=>Pcb.request({},values()),/Choose a copper layer/);
  assert.throws(()=>Pcb.request({copper:'x'.repeat(4*1024*1024+1)},values()),/4 MiB/);
  const large='x'.repeat(3*1024*1024);
  assert.throws(()=>Pcb.request({copper:large,outline:large,drill:large},values()),/together are larger than 7 MiB/);
  assert.throws(()=>Pcb.request(files,values({side:'left'})),/top or the underside/);
  assert.throws(()=>Pcb.request(files,values({angle:5})),/V-bit angle must be between 10 and 170/);
  assert.throws(()=>Pcb.request(files,values({depth:null})),/Engraving depth must be a number/);
  assert.throws(()=>Pcb.request(files,values({passes:1.5})),/whole number/);
  assert.throws(()=>Pcb.request(files,values({overlapPercent:95})),/Overlap must be between 0 and 90/);
  assert.throws(()=>Pcb.request(files,values({integer:3})),/both the integer and the decimal digits/);
  assert.throws(()=>Pcb.request({copper:'G04*'},values({clip:true})),/needs the board outline/);
});

test('a second import registers with the board already in the design',()=>{
  const registered={offset:[-95,115],outline_bounds:[5,5,45,35]};
  const under=Pcb.request({copper:'G04*'},values({side:'bottom',registered,margin:99}));
  assert.deepEqual(under.offset,[-95,115]);assert.equal(under.mirror_x,25);assert.equal(under.margin,undefined);
  // With the outline in the request the server finds the mirror line itself.
  assert.equal(Pcb.request({copper:'G04*',outline:'G04*'},values({side:'bottom',registered})).mirror_x,undefined);
  assert.equal(Pcb.request({copper:'G04*'},values({registered})).mirror_x,undefined);
});

test('how a drill file was read is put in words',()=>{
  const read=answer().drill.interpretation;
  assert.equal(Pcb.formatText(read),'millimetres (from the header); 3.3 digits (from the default); leading zeros omitted (from the header)');
  assert.equal(Pcb.formatGuessed(read),true);
  const decimal={units:'inch',units_from:'request',digits_from:'decimal points',zeros_from:'decimal points'};
  assert.equal(Pcb.formatText(decimal),'inches (from the request); digits and zeros not needed: every number has a decimal point');
  assert.equal(Pcb.formatGuessed(decimal),false);
  const lines=Pcb.summary(answer());
  assert.equal(lines.length,4);
  assert.match(lines[0],/^Copper \(Copper,L1,Top\): 2 flashes, 1 lines, 0 arcs, 0 regions in millimetres; 2 separate pieces of copper; 2 contours for a tool cutting 0\.15359 mm wide$/);
  assert.match(lines[1],/^Outline: 2 closed loops, 30 × 30 mm$/);
  assert.match(lines[2],/Holes: 2 × 0\.4 mm, 1 × 0\.8 mm; 2 slots$/);
  assert.match(lines[3],/^Placed with offset X-95 Y115; it spans X5 to X45 and Y5 to Y35$/);
  assert.match(Pcb.summary(answer({mirror_x:25}))[3],/^Placed mirrored about X25, with offset/);
  assert.deepEqual(Pcb.warnings(answer({warnings:['Holes lie outside the board outline']})),
    ['Copper: The cutter cannot pass between separate copper features in 1 place','Outline: The outline has 2 closed loops',
      'Drill file: State "integer" and "decimal" in the request if this is wrong','Holes lie outside the board outline']);
});

test('a board becomes one layer for each tool, in cutting order',()=>{
  const staged=Pcb.plan(answer(),values({firstTool:4}));
  assert.deepEqual(staged.layers.map(l=>l.name),['PCB top · isolation','PCB top · drill 0.4 mm','PCB top · drill 0.8 mm','PCB top · slots 1 mm','PCB top · cut-out']);
  assert.deepEqual(staged.objects.map(o=>[o.name,o.sourceLayer,o.operationSettings.tool_id,o.operationSettings.operation,o.operationSettings.tool_shape]),[
    ['Isolation contours 1','isolation',4,'line','v'],['Isolation contours 2','isolation',4,'line','v'],
    ['Drill 0.4 mm · 2 holes','drill-0.4',5,'drill','drill'],['Drill 0.8 mm · 1 hole','drill-0.8',6,'drill','drill'],
    ['Slots 1 mm · 2','slots-1',7,'line','flat'],['Board outline','cut-out',8,'outside','flat']]);
  assert.equal(staged.tools,5);
  const [isolation,,small,,slots,outline]=staged.objects.map(o=>o.operationSettings);
  assert.deepEqual(isolation,{mode:'router',router_entry:'plunge',operation:'line',tool_shape:'v',tool_diameter:0.15359,depth:0.1,step:0.1,tool_id:4,tool_name:'V-bit 30° cutting 0.15359 mm',v_angle:30});
  assert.deepEqual([small.tool_diameter,small.depth,small.tool_name,small.drill_point_angle],[0.4,1.8,'Drill 0.4 mm',118]);
  assert.deepEqual([slots.tool_diameter,slots.depth,slots.step],[1,1.8,0.6]);
  assert.deepEqual([outline.tool_diameter,outline.depth,outline.step,outline.tool_name],[1.5,1.8,0.6,'End mill 1.5 mm']);
  // A hole is a closed circle of the drill's diameter about its centre, which is what the drill operation reads.
  const hole=staged.objects[2].paths[1];
  assert.equal(hole.length,33);assert.deepEqual(hole[0],hole[32]);
  for(const [x,y] of hole)assert.ok(Math.abs(Math.hypot(x-15.5,y-29.75)-0.2)<1e-12);
  const job=generate({mode:'router',width:60,height:40,feed:100,plunge:60,power:12000,safe:3,...small,paths:staged.objects[2].paths});
  assert.deepEqual(job.drill_holes.map(([x,y,d])=>[Math.round(x*1e6)/1e6,Math.round(y*1e6)/1e6,d]),[[10,30,0.4],[15.5,29.75,0.4]]);
  assert.equal(staged.paths,2+2+1+2+2);
  assert.equal(staged.points,5+5+33+33+33+2+2+5+5);
  // The underside is named, an engraver with a stated width is a flat tool, and tools number from one by default.
  const under=Pcb.plan(answer({side:'bottom',mirror_x:25,drill:null,outline:null}),values({kind:'width'}));
  assert.deepEqual(under.layers.map(l=>l.name),['PCB underside · isolation']);
  assert.deepEqual([under.objects[0].operationSettings.tool_shape,under.objects[0].operationSettings.tool_id,under.objects[0].operationSettings.v_angle],['flat',1,undefined]);
  assert.throws(()=>Pcb.plan(answer(),values({drillDepth:null})),/Drill depth must be a number/);
  assert.throws(()=>Pcb.plan(answer(),values({cutter:0})),/Cut-out cutter must be between/);
});

test('the project keeps where the board was placed and the places the cutter cannot reach',()=>{
  const kept=Pcb.record(answer(),['pcb_a','pcb_b']);
  assert.deepEqual(kept,{side:'top',offset:[-95,115],mirror_x:null,outline_bounds:[5,5,35,35],tool_width:0.15359,layers:['pcb_a','pcb_b'],unreachable:[{at:[20,20],gap:0.12}]});
  assert.deepEqual(Pcb.record(answer({copper:null,outline:null}),[]).unreachable,[]);
  assert.equal(Pcb.record(answer({copper:null,outline:null}),[]).outline_bounds,null);
});
