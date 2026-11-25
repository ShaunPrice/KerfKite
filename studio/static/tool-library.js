/* Tool library: the cutters an operator owns, and starting feeds and speeds worked out from chip load.
   The calculation is pure and also runs under Node. Applying a tool only fills the CAM fields of the editor:
   nothing here reaches a controller. Distances are millimetres, feeds mm/min, spindle speed rev/min. */
((root)=>{
'use strict';
const FORMAT='kerfkite-tools/1';
const SHAPES={flat:'Flat end mill',ball:'Ball end mill',v:'V bit',drill:'Drill bit',surfacing:'Surfacing bit'};
/* For each material: chip load per tooth as a fraction of the cutter diameter, the cutting speed at the edge
   (m/min) that sets the spindle speed, depth of cut as a fraction of the diameter, and the plunge as a fraction
   of the feed. These are conservative starting values for a rigid machine; the rigidity setting scales chip load
   and depth for lighter ones. They are not a manufacturer's figures: cut a test piece first. */
const MATERIALS=[
 {id:'softwood',name:'Softwood',chip:.016,speed:400,depth:.5,plunge:.35},
 {id:'hardwood',name:'Hardwood',chip:.0125,speed:400,depth:.4,plunge:.3},
 {id:'plywood',name:'Plywood',chip:.014,speed:400,depth:.5,plunge:.3},
 {id:'mdf',name:'MDF',chip:.016,speed:400,depth:.5,plunge:.35},
 {id:'acrylic',name:'Cast acrylic',chip:.015,speed:250,depth:.4,plunge:.3},
 {id:'hdpe',name:'HDPE and soft plastics',chip:.019,speed:250,depth:.5,plunge:.35},
 {id:'foam',name:'Rigid foam',chip:.025,speed:400,depth:1,plunge:.5},
 {id:'aluminium',name:'Aluminium',chip:.004,speed:200,depth:.1,plunge:.2},
 {id:'brass',name:'Brass',chip:.003,speed:150,depth:.08,plunge:.2},
 {id:'pcb',name:'Copper-clad board (FR-4)',chip:.006,speed:150,depth:.2,plunge:.25},
];
const RIGIDITY={light:{name:'Light · belt or small rail machine',factor:.5},medium:{name:'Medium · hobby gantry router',factor:.75},rigid:{name:'Rigid · steel or cast machine',factor:1}};
const CUTS={profile:'Profile or slot',pocket:'Pocket clearing',finish:'Finishing pass'};
const STARTERS=[
 {name:'3.175 mm (1/8 in) two-flute end mill',shape:'flat',diameter:3.175,flutes:2},
 {name:'6.35 mm (1/4 in) two-flute end mill',shape:'flat',diameter:6.35,flutes:2},
 {name:'6 mm single-flute end mill',shape:'flat',diameter:6,flutes:1},
 {name:'2 mm two-flute end mill',shape:'flat',diameter:2,flutes:2},
 {name:'3.175 mm (1/8 in) ball end mill',shape:'ball',diameter:3.175,flutes:2},
 {name:'6.35 mm (1/4 in) ball end mill',shape:'ball',diameter:6.35,flutes:2},
 {name:'60° V bit, 12.7 mm',shape:'v',diameter:12.7,flutes:2,angle:60},
 {name:'90° V bit, 12.7 mm',shape:'v',diameter:12.7,flutes:2,angle:90},
 {name:'30° engraving bit, 3.175 mm shank',shape:'v',diameter:3.175,flutes:1,angle:30},
 {name:'25.4 mm (1 in) surfacing bit',shape:'surfacing',diameter:25.4,flutes:2},
 {name:'3 mm twist drill',shape:'drill',diameter:3,flutes:2,angle:118},
 {name:'0.8 mm circuit-board drill',shape:'drill',diameter:.8,flutes:2,angle:130},
];
const DEFAULT_MACHINE={rigidity:'medium',minRpm:10000,maxRpm:24000,maxFeed:2000};

function number(value,label,low,high,integer=false){
 const n=typeof value==='string'&&value.trim()!==''?Number(value):value;
 if(typeof n!=='number'||!Number.isFinite(n)||n<low||n>high||(integer&&!Number.isInteger(n)))throw Error(`${label} must be ${integer?'a whole number ':''}between ${low} and ${high}`);
 return n;
}
function tool(raw){
 if(!raw||typeof raw!=='object'||Array.isArray(raw))throw Error('A tool must be an object');
 const name=typeof raw.name==='string'?raw.name.trim():'';
 if(!name||name.length>80||/[\x00-\x1f]/.test(name))throw Error('Give the tool a name of 1 to 80 characters');
 if(!Object.hasOwn(SHAPES,raw.shape))throw Error('Choose a flat, ball, V, drill or surfacing tool');
 const out={name,shape:raw.shape,diameter:number(raw.diameter,'Cutter diameter',.05,100),flutes:number(raw.flutes??2,'Flutes',1,8,true)};
 if(raw.shape==='v'||raw.shape==='drill')out.angle=number(raw.angle??(raw.shape==='v'?60:118),'Included angle',10,170);
 if(raw.number!==undefined&&raw.number!==null&&raw.number!=='')out.number=number(raw.number,'Tool number',1,999,true);
 if(typeof raw.notes==='string'&&raw.notes.trim())out.notes=raw.notes.trim().slice(0,200);
 return out;
}
function machine(raw={}){
 const m={...DEFAULT_MACHINE,...raw};
 if(!Object.hasOwn(RIGIDITY,m.rigidity))throw Error('Choose a light, medium or rigid machine');
 const out={rigidity:m.rigidity,minRpm:number(m.minRpm,'Lowest spindle speed',100,100000),maxRpm:number(m.maxRpm,'Highest spindle speed',100,100000),maxFeed:number(m.maxFeed,'Highest cutting feed',10,50000)};
 if(out.minRpm>out.maxRpm)throw Error('The lowest spindle speed is above the highest');
 return out;
}
function library(raw){
 if(!raw||raw.format!==FORMAT||!Array.isArray(raw.tools))throw Error('This is not a KerfKite tool library');
 if(raw.tools.length>200)throw Error('A tool library holds at most 200 tools');
 const tools=raw.tools.map(tool),names=new Set(tools.map(t=>t.name.toLowerCase()));
 if(names.size!==tools.length)throw Error('Two tools in the library have the same name');
 return {format:FORMAT,tools,machine:machine(raw.machine)};
}
const starter=()=>library({format:FORMAT,tools:STARTERS,machine:DEFAULT_MACHINE});
const round=(value,step)=>Math.round(value/step)*step;
const trim=value=>Number(value.toFixed(3));

/* Starting settings for one tool in one material.
   The spindle speed comes from the material's cutting speed, held inside the machine's range. The feed is
   speed × flutes × chip load. When the machine cannot feed that fast the spindle slows to keep the chip, down
   to its lowest speed; below that the chip is thinner than intended and the answer says so. */
function recommend(rawTool,materialId,rawMachine,options={}){
 const t=tool(rawTool),m=machine(rawMachine),material=MATERIALS.find(x=>x.id===materialId);
 if(!material)throw Error('Choose a material from the list');
 const cut=options.cut??'profile';
 if(!Object.hasOwn(CUTS,cut))throw Error('Choose a profile, pocket or finishing cut');
 const rigidity=RIGIDITY[m.rigidity].factor,warnings=[];
 // A V bit cuts on the part of its cone that is in the stock; a drill and the others on their full diameter.
 let working=t.diameter;
 if(t.shape==='v'){
   const depth=number(options.depth??1,'Cut depth',.01,50);
   working=Math.min(t.diameter,Math.max(.5,2*depth*Math.tan(t.angle*Math.PI/360)));
 }
 const speedDiameter=t.shape==='v'?Math.max(working,1):t.diameter;
 let rpm=material.speed*1000/(Math.PI*speedDiameter);
 rpm=Math.min(m.maxRpm,Math.max(m.minRpm,rpm));
 let chip=material.chip*working*rigidity*(cut==='finish'?.6:1);
 if(t.shape==='surfacing')chip=Math.min(chip,.08);  // wide cutters are limited by the spindle, not the edge
 if(t.shape==='drill')chip=material.chip*t.diameter*rigidity*.5;  // advance per lip for each revolution
 let feed=rpm*t.flutes*chip;
 if(feed>m.maxFeed){
   feed=m.maxFeed;
   const wanted=feed/(t.flutes*chip);
   rpm=Math.max(m.minRpm,wanted);
   if(wanted<m.minRpm*.7)warnings.push('The machine cannot feed fast enough for this cutter at its lowest spindle speed: the chip is thinner than intended and the cutter may rub. Use a cutter with fewer flutes or a smaller one.');
 }
 rpm=round(rpm,100);feed=Math.max(1,round(feed,5));
 const actualChip=feed/(rpm*t.flutes);
 let depth,stepover,plunge=Math.max(1,round(feed*material.plunge,5));
 if(t.shape==='drill'){
   // A drill only plunges: its feed is the plunge, and it pecks by its own diameter.
   plunge=feed;depth=trim(t.diameter);stepover=trim(t.diameter);
 }else if(t.shape==='v'){
   depth=trim(Math.max(.05,Math.min(working*.5,3)*rigidity));stepover=trim(Math.max(.05,working*.2));
 }else if(t.shape==='surfacing'){
   depth=trim(Math.min(1,.5*rigidity+.25));stepover=trim(t.diameter*.6);
 }else{
   depth=trim(Math.max(.05,t.diameter*material.depth*rigidity*(cut==='finish'?.5:1)));
   stepover=trim(t.shape==='ball'?t.diameter*(cut==='finish'?.1:.3):t.diameter*(cut==='finish'?.1:.4));
 }
 if(t.diameter<1.2&&t.shape!=='v')warnings.push('A cutter this small breaks easily: check its runout and start below these values.');
 if(material.id==='aluminium'||material.id==='brass')warnings.push('Metal needs a rigid machine, a firm clamp and chips cleared from the cut; lubricate aluminium.');
 if(m.maxRpm<material.speed*1000/(Math.PI*speedDiameter)*.5&&t.shape!=='drill')warnings.push('The spindle is slower than this cutter would like in this material; the feed has been lowered to match.');
 return {tool:t,material:material.id,cut,rpm,feed,plunge,depth_per_pass:depth,stepover,chip_load:Number(actualChip.toFixed(4)),working_diameter:trim(working),warnings};
}
/* The CAM fields a recommendation fills in the editor. */
function fields(result){
 const out={tool_diameter:result.tool.diameter,feed:result.feed,plunge:result.plunge,power:result.rpm,step:result.depth_per_pass,stepover:result.stepover};
 if(result.tool.shape==='v')out.v_angle=result.tool.angle;
 if(result.tool.shape==='drill'){out.drill_point_angle=result.tool.angle;out.drill_peck=result.depth_per_pass;}
 return out;
}
const api={FORMAT,SHAPES,MATERIALS,RIGIDITY,CUTS,tool,machine,library,starter,recommend,fields};
if(typeof module!=='undefined')module.exports=api;
root.KerfToolLibrary=api;
if(typeof document==='undefined'||!document.querySelector)return;

const $=id=>document.getElementById(id);
const panel=document.createElement('details');panel.id='toolLibraryPanel';
const options=object=>Object.entries(object).map(([value,label])=>`<option value="${value}">${typeof label==='string'?label:label.name}</option>`).join('');
panel.innerHTML=`<summary>Tool library · feeds and speeds</summary>
<p class="hint">Choose a cutter and a material for starting values worked out from chip load. They are a place to begin, not a tested recipe: cut a sample, listen to the cut and adjust. Router mode only.</p>
<label>Tool<select id="toolLibraryTool"></select></label>
<div class="pair"><label>Material<select id="toolLibraryMaterial">${MATERIALS.map(m=>`<option value="${m.id}">${m.name}</option>`).join('')}</select></label><label>Cut<select id="toolLibraryCut">${options(CUTS)}</select></label></div>
<details id="toolLibraryMachine"><summary>This machine</summary><label>Rigidity<select id="toolLibraryRigidity">${options(RIGIDITY)}</select></label><div class="pair"><label>Lowest spindle speed · rev/min<input id="toolLibraryMinRpm" type="number" min="100" max="100000" step="100"></label><label>Highest spindle speed · rev/min<input id="toolLibraryMaxRpm" type="number" min="100" max="100000" step="100"></label></div><label>Highest cutting feed · mm/min<input id="toolLibraryMaxFeed" type="number" min="10" max="50000" step="10"></label></details>
<div id="toolLibraryResult" class="hint" role="status"></div>
<button id="toolLibraryApply" type="button" class="primary">Use these settings</button>
<details id="toolLibraryEdit"><summary>Add or change a tool</summary><label>Name<input id="toolLibraryName" maxlength="80" autocomplete="off"></label><div class="pair"><label>Shape<select id="toolLibraryShape">${options(SHAPES)}</select></label><label>Diameter · mm<input id="toolLibraryDiameter" type="number" min=".05" max="100" step=".001"></label><label>Flutes<input id="toolLibraryFlutes" type="number" min="1" max="8" step="1"></label><label>Included angle · degrees<input id="toolLibraryAngle" type="number" min="10" max="170" step="1"></label><label>Tool number (optional)<input id="toolLibraryNumber" type="number" min="1" max="999" step="1"></label></div><div class="buttons"><button id="toolLibrarySave" type="button">Save tool</button><button id="toolLibraryDelete" type="button" class="danger">Delete tool</button></div><div class="buttons"><button id="toolLibraryExport" type="button">Export library…</button><button id="toolLibraryImport" type="button">Import library…</button><input id="toolLibraryFile" type="file" accept=".json,application/json" hidden></div></details>`;
document.querySelector('aside.operations')?.append(panel);
if(!panel.isConnected)return;
let key='kerfkite-tool-library',data=starter();
const fail=e=>root.message?.(e.message,true);
const guard=fn=>(...a)=>{try{return fn(...a);}catch(e){fail(e);}};
function store(){try{localStorage.setItem(key,JSON.stringify(data));}catch{root.message?.('The tool library could not be saved in this browser; export it to keep it.',true);}}
function load(){try{const text=localStorage.getItem(key);if(text)data=library(JSON.parse(text));}catch{data=starter();}}
function current(){return data.tools[$('toolLibraryTool').selectedIndex]||data.tools[0];}
function readMachine(){return machine({rigidity:$('toolLibraryRigidity').value,minRpm:$('toolLibraryMinRpm').value,maxRpm:$('toolLibraryMaxRpm').value,maxFeed:$('toolLibraryMaxFeed').value});}
function showMachine(){const m=data.machine;$('toolLibraryRigidity').value=m.rigidity;$('toolLibraryMinRpm').value=m.minRpm;$('toolLibraryMaxRpm').value=m.maxRpm;$('toolLibraryMaxFeed').value=m.maxFeed;}
function showTools(selectName){
 const select=$('toolLibraryTool');select.replaceChildren(...data.tools.map(t=>new Option(`${t.number?'T'+t.number+' · ':''}${t.name}`,t.name)));
 if(selectName)select.value=selectName;
 showTool();
}
function showTool(){
 const t=current();if(!t)return;
 $('toolLibraryName').value=t.name;$('toolLibraryShape').value=t.shape;$('toolLibraryDiameter').value=t.diameter;$('toolLibraryFlutes').value=t.flutes;$('toolLibraryAngle').value=t.angle??'';$('toolLibraryNumber').value=t.number??'';
 show();
}
let latest=null;
function show(){
 latest=null;
 try{
   latest=recommend(current(),$('toolLibraryMaterial').value,readMachine(),{cut:$('toolLibraryCut').value,depth:Number($('depth')?.value)||1});
   const r=latest;
   $('toolLibraryResult').textContent=`${r.rpm.toLocaleString('en')} rev/min · feed ${r.feed} mm/min · plunge ${r.plunge} mm/min · ${r.tool.shape==='drill'?'peck':'depth per pass'} ${r.depth_per_pass} mm${r.tool.shape==='drill'?'':` · stepover ${r.stepover} mm`} · chip ${r.chip_load} mm per tooth.${r.warnings.length?' '+r.warnings.join(' '):''}`;
 }catch(e){$('toolLibraryResult').textContent=e.message;}
}
for(const id of ['toolLibraryMaterial','toolLibraryCut','toolLibraryRigidity','toolLibraryMinRpm','toolLibraryMaxRpm','toolLibraryMaxFeed'])$(id).addEventListener('change',guard(()=>{if(id!=='toolLibraryMaterial'&&id!=='toolLibraryCut'){data.machine=readMachine();store();}show();}));
$('toolLibraryTool').addEventListener('change',showTool);
$('depth')?.addEventListener('change',show);
$('toolLibraryApply').onclick=guard(()=>{
 if($('mode').value!=='router')throw Error('Feeds and speeds from the tool library are for router mode');
 if(!latest)throw Error($('toolLibraryResult').textContent||'Choose a tool and a material');
 for(const [field,value] of Object.entries(fields(latest)))if($(field))$(field).value=value;
 // The manual tool-change panel takes the same identity, ready to assign.
 const shape=latest.tool.shape==='surfacing'?'flat':latest.tool.shape;
 if($('manualToolName')){$('manualToolName').value=latest.tool.name;$('manualToolShape').value=shape;if(latest.tool.number)$('manualToolNumber').value=latest.tool.number;}
 root.modeUI?.();root.invalidate?.();
 root.message?.(`${latest.tool.name}: starting settings entered. Check the depth of cut for this job and test on scrap.`);
});
$('toolLibrarySave').onclick=guard(()=>{
 const t=tool({name:$('toolLibraryName').value,shape:$('toolLibraryShape').value,diameter:$('toolLibraryDiameter').value,flutes:$('toolLibraryFlutes').value,angle:$('toolLibraryAngle').value||undefined,number:$('toolLibraryNumber').value});
 const at=data.tools.findIndex(x=>x.name.toLowerCase()===t.name.toLowerCase());
 if(at<0&&data.tools.length>=200)throw Error('A tool library holds at most 200 tools');
 if(at<0)data.tools.push(t);else data.tools[at]=t;
 store();showTools(t.name);root.message?.(`${t.name} saved in the tool library`);
});
$('toolLibraryDelete').onclick=guard(()=>{
 if(data.tools.length<2)throw Error('Keep at least one tool in the library');
 const t=current();if(!confirm(`Delete ${t.name} from the tool library?`))return;
 data.tools=data.tools.filter(x=>x!==t);store();showTools();
});
$('toolLibraryExport').onclick=guard(()=>root.download?.('kerfkite-tools.json',JSON.stringify(data,null,1),'application/json'));
$('toolLibraryImport').onclick=()=>$('toolLibraryFile').click();
$('toolLibraryFile').onchange=async()=>{
 const file=$('toolLibraryFile').files[0];$('toolLibraryFile').value='';if(!file)return;
 try{if(file.size>200000)throw Error('The tool library file is too large');data=library(JSON.parse(await file.text()));store();showMachine();showTools();root.message?.(`${data.tools.length} tools imported`);}catch(e){fail(e);}
};
const begin=session=>{if(session?.username)key+=':'+session.username;load();showMachine();showTools();};
if(root.studioReady?.then)root.studioReady.then(begin,()=>begin());else begin();
})(typeof window!=='undefined'?window:globalThis);
