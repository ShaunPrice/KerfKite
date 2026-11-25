/* Machine setup tools on the Control tab: zeroing on a touch plate, the controller's settings, axis calibration,
   a spoilboard surfacing job and operator macros. The calculations are pure and also run under Node. Everything
   that moves the machine or writes to the controller goes through the workstation server's setup routes, needs
   the controller idle and asks first. Millimetres throughout. */
((root)=>{
'use strict';
const MACRO_FORMAT='kerfkite-macros/1';
/* The Grbl settings most often changed during commissioning. Others are listed by number alone. */
const SETTING_NAMES={0:'Step pulse time · µs',1:'Step idle delay · ms',2:'Step pulse invert · mask',3:'Step direction invert · mask',4:'Invert step enable pin',5:'Invert limit pins',6:'Invert probe pin',10:'Status report options · mask',11:'Junction deviation · mm',12:'Arc tolerance · mm',13:'Report in inches',20:'Soft limits',21:'Hard limits',22:'Homing cycle',23:'Homing direction invert · mask',24:'Homing locate feed · mm/min',25:'Homing search feed · mm/min',26:'Homing switch debounce · ms',27:'Homing pull-off · mm',30:'Maximum spindle speed · rev/min',31:'Minimum spindle speed · rev/min',32:'Laser mode',100:'X steps per mm',101:'Y steps per mm',102:'Z steps per mm',110:'X maximum rate · mm/min',111:'Y maximum rate · mm/min',112:'Z maximum rate · mm/min',120:'X acceleration · mm/s²',121:'Y acceleration · mm/s²',122:'Z acceleration · mm/s²',130:'X maximum travel · mm',131:'Y maximum travel · mm',132:'Z maximum travel · mm'};
const STEPS={X:'100',Y:'101',Z:'102'};
function number(value,label,low,high){
 const n=typeof value==='string'&&value.trim()!==''?Number(value):value;
 if(typeof n!=='number'||!Number.isFinite(n)||n<low||n>high)throw Error(`${label} must be between ${low} and ${high}`);
 return n;
}
/* Steps per millimetre after a calibration move: the axis was told to move `commanded` and moved `measured`. */
function calibrate(steps,commanded,measured){
 const s=number(steps,'Current steps per mm',.001,100000),c=number(commanded,'Commanded distance',1,5000),m=number(measured,'Measured distance',.01,5000);
 const next=s*c/m,change=(next/s-1)*100;
 // A correction beyond a fifth is more likely a mistaken measurement or a wrong pulley than a calibration.
 if(Math.abs(change)>20)throw Error(`That would change the steps per mm by ${change.toFixed(1)}%. Check the measurement: a calibration corrects a few per cent at most`);
 return {steps:Number(next.toFixed(3)),change:Number(change.toFixed(3))};
}
/* A surfacing pass over width × height from the origin with a flat cutter: back and forth along the longer side,
   then once round the edge. The cutter's centre stays its radius inside the area, so nothing is cut outside it.
   Returns the path and the pitch between passes, which is at most the stepover asked for. */
function surfacing(width,height,diameter,stepover){
 const w=number(width,'Surfacing width',1,5000),h=number(height,'Surfacing height',1,5000),d=number(diameter,'Cutter diameter',1,150),over=number(stepover,'Stepover',.1,150);
 if(over>d)throw Error('The stepover must not be more than the cutter diameter, or strips are left uncut');
 const r=d/2;
 if(w<d||h<d)throw Error('The area is smaller than the cutter');
 const along=w>=h;  // passes run along the longer side
 const span=(along?h:w)-d,passes=Math.max(1,Math.ceil(span/over-1e-9))+1,pitch=passes>1?span/(passes-1):0;
 const path=[];
 for(let i=0;i<passes;i++){
   const across=r+pitch*i,ends=i%2?[(along?w:h)-r,r]:[r,(along?w:h)-r];
   for(const e of ends)path.push(along?[e,across]:[across,e]);
 }
 // Once round the edge leaves a clean boundary whichever way the last pass ended.
 const last=path[path.length-1],ring=[[r,r],[w-r,r],[w-r,h-r],[r,h-r]];
 let start=0,best=Infinity;ring.forEach((p,i)=>{const dist=Math.hypot(p[0]-last[0],p[1]-last[1]);if(dist<best){best=dist;start=i;}});
 for(let k=0;k<=4;k++){const p=ring[(start+k)%4];if(Math.hypot(p[0]-path[path.length-1][0],p[1]-path[path.length-1][1])>1e-9)path.push([...p]);}
 return {path,passes,pitch:Number(pitch.toFixed(4))};
}
/* The lines of a macro as the server will accept them: plain G-code words, notes after a semicolon dropped. */
function macroLines(text){
 if(typeof text!=='string')throw Error('A macro is text, one G-code line to a row');
 const out=[];
 for(const raw of text.split(/\r?\n/)){
   const line=raw.split(';')[0].trim().toUpperCase();
   if(!line)continue;
   if(line.length>78)throw Error('A macro line has at most 78 characters');
   if(!/^[A-Z0-9 .+\-]+$/.test(line))throw Error('A macro holds plain G-code words only: letters, numbers, spaces and . + -');
   if(!/^[A-Z]/.test(line))throw Error('A macro line starts with a G-code word, such as G0 or M5');
   out.push(line);
 }
 if(!out.length)throw Error('The macro has no G-code in it');
 if(out.length>100)throw Error('A macro has 1 to 100 lines');
 return out;
}
function macro(raw){
 if(!raw||typeof raw!=='object')throw Error('A macro must be an object');
 const name=typeof raw.name==='string'?raw.name.trim():'';
 if(!name||name.length>60||/[\x00-\x1f]/.test(name))throw Error('Give the macro a name of 1 to 60 characters');
 if(typeof raw.text!=='string'||raw.text.length>8000)throw Error('A macro is text, one G-code line to a row');
 macroLines(raw.text);
 return {name,text:raw.text};
}
function macroLibrary(raw){
 if(!raw||raw.format!==MACRO_FORMAT||!Array.isArray(raw.macros))throw Error('This is not a KerfKite macro library');
 if(raw.macros.length>100)throw Error('A macro library holds at most 100 macros');
 const macros=raw.macros.map(macro);
 if(new Set(macros.map(m=>m.name.toLowerCase())).size!==macros.length)throw Error('Two macros in the library have the same name');
 return {format:MACRO_FORMAT,macros};
}
const STARTER_MACROS=[
 {name:'Go to work zero, raised',text:'G90\nG0 Z5 ; lift first\nG0 X0 Y0'},
 {name:'Spindle and coolant off',text:'M5\nM9'},
 {name:'Raise Z 20 mm',text:'G91\nG0 Z20\nG90'},
 {name:'Dwell 2 seconds',text:'G4 P2'},
];
const starterMacros=()=>macroLibrary({format:MACRO_FORMAT,macros:STARTER_MACROS});
const core={SETTING_NAMES,STEPS,MACRO_FORMAT,calibrate,surfacing,macroLines,macro,macroLibrary,starterMacros};
if(typeof module!=='undefined')module.exports=core;
root.KerfSetupTools=core;
if(typeof document==='undefined'||!document.querySelector)return;

const $=id=>document.getElementById(id);
const machine=$('machinePanel');
if(!machine||(typeof offline!=='undefined'&&offline))return;
const box=document.createElement('details');box.id='setupToolsPanel';box.hidden=true;
box.innerHTML=`<summary>Machine setup tools</summary>
<p class="hint">These tools move the machine or change its controller. Each needs the controller connected and idle, and asks before it acts. Keep a hand near the stop.</p>
<details id="setupProbe"><summary>Zero on a touch plate</summary>
<p class="hint">Put the plate on the front-left corner of the stock with its lead connected, and the clip on the cutter. Touch the clip to the plate and check that the controller reports the probe before trusting it. For Z alone, place the cutter over the plate. For X, Y and Z, place it over the plate within the distance given of the plate's left and front faces, with room to its left and in front. Needs a Grbl-family controller.</p>
<label>Probe<select id="setupProbeAxes"><option value="z">Z only · plate flat on the stock</option><option value="xyz">X, Y and Z · corner plate</option></select></label>
<div class="pair"><label>Plate thickness · mm<input id="setupProbeThickness" type="number" min=".1" max="50" step=".01" value="15"></label><label>Probe feed · mm/min<input id="setupProbeFeed" type="number" min="10" max="500" step="10" value="150"></label></div>
<div class="pair" id="setupProbeCorner" hidden><label>Plate wall, face to stock edge · mm<input id="setupProbeWall" type="number" min="0" max="50" step=".01" value="10"></label><label>Cutter diameter · mm<input id="setupProbeTool" type="number" min=".1" max="25" step=".001"></label><label>Cutter is within this of the plate's faces · mm<input id="setupProbeInset" type="number" min="5" max="40" step="1" value="15"></label></div>
<label class="check"><input type="checkbox" id="setupProbeConfirmed"> Plate in place, lead tested, spindle stopped</label>
<button id="setupProbeRun" type="button">Probe and set work zero</button><div id="setupProbeState" class="hint" role="status"></div></details>
<details id="setupSettings"><summary>Controller settings</summary>
<p class="hint">The settings the controller reported when it connected. Changing one writes it to the controller at once and reads every setting back. Export them first: a wrong value can make the machine move wrongly.</p>
<div class="buttons"><button id="setupSettingsRefresh" type="button">Show settings</button><button id="setupSettingsExport" type="button">Export settings…</button></div>
<div id="setupSettingsList" style="max-height:260px;overflow:auto"></div></details>
<details id="setupCalibrate"><summary>Calibrate an axis</summary>
<p class="hint">Mark the axis, move it a known distance with the jog buttons or a macro, and measure how far it went. The steps per millimetre are corrected by commanded ÷ measured. A longer move gives a better correction.</p>
<div class="pair"><label>Axis<select id="setupCalAxis"><option>X</option><option>Y</option><option>Z</option></select></label><label>Current steps per mm<input id="setupCalSteps" type="number" min=".001" step=".001"></label><label>Commanded distance · mm<input id="setupCalCommanded" type="number" min="1" step="1" value="100"></label><label>Measured distance · mm<input id="setupCalMeasured" type="number" min=".01" step=".01" value="100"></label></div>
<div id="setupCalResult" class="hint" role="status"></div><button id="setupCalWrite" type="button">Write the corrected steps per mm</button></details>
<details id="setupSurface"><summary>Surface the spoilboard</summary>
<p class="hint">Adds a surfacing job to the design: passes back and forth over the area, then once round its edge. It uses router mode and the CAM feed, spindle speed and depth. Set work zero at the front-left corner of the area and Z zero on its highest point, and preview before running.</p>
<div class="pair"><label>Width · mm<input id="setupSurfaceWidth" type="number" min="1" step="1"></label><label>Height · mm<input id="setupSurfaceHeight" type="number" min="1" step="1"></label><label>Cutter diameter · mm<input id="setupSurfaceTool" type="number" min="1" max="150" step=".1" value="25.4"></label><label>Stepover · mm<input id="setupSurfaceStep" type="number" min=".1" step=".1" value="15"></label><label>Depth to remove · mm<input id="setupSurfaceDepth" type="number" min=".05" max="5" step=".05" value=".5"></label></div>
<button id="setupSurfaceAdd" type="button">Add surfacing job to the design</button></details>
<details id="setupMacros"><summary>Macros</summary>
<p class="hint">Short G-code programs of your own, sent to the idle controller when you choose Run. Plain G-code words only; a note may follow a semicolon. Macros stay in this browser: export them to keep them.</p>
<label>Macro<select id="setupMacroList"></select></label><label>Name<input id="setupMacroName" maxlength="60" autocomplete="off"></label>
<label>G-code<textarea id="setupMacroText" rows="5" spellcheck="false" autocapitalize="off"></textarea></label>
<div class="buttons"><button id="setupMacroRun" type="button">Run macro</button><button id="setupMacroSave" type="button">Save macro</button><button id="setupMacroDelete" type="button" class="danger">Delete macro</button></div>
<div class="buttons"><button id="setupMacroExport" type="button">Export macros…</button><button id="setupMacroImport" type="button">Import macros…</button><input id="setupMacroFile" type="file" accept=".json,application/json" hidden></div></details>`;
machine.append(box);
const task=root.task;
const settle=async()=>{for(;;){await new Promise(r=>setTimeout(r,300));const status=await api('status');if(!status.active)return status;}};

// ---- touch plate ----
const syncTool=()=>{$('setupProbeTool').value=Number($('tool_diameter')?.value)||3.175;};
syncTool();$('tool_diameter')?.addEventListener('change',syncTool);
$('setupProbeAxes').onchange=()=>{$('setupProbeCorner').hidden=$('setupProbeAxes').value!=='xyz';};
$('setupProbeRun').onclick=task(async()=>{
 if(!$('setupProbeConfirmed').checked)throw Error('Confirm the plate setup first');
 const axes=$('setupProbeAxes').value,request={confirmed:true,axes,thickness:Number($('setupProbeThickness').value),feed:Number($('setupProbeFeed').value)};
 if(axes==='xyz')Object.assign(request,{wall:Number($('setupProbeWall').value),tool_diameter:Number($('setupProbeTool').value),inset:Number($('setupProbeInset').value)});
 await api('setup/probe',request);
 $('setupProbeConfirmed').checked=false;$('setupProbeState').textContent='Probing; keep clear of the machine';
 const status=await settle();
 if(status.error){$('setupProbeState').textContent=`Probing stopped: ${status.error}. Work zero was not set completely; check the plate and probe again.`;throw Error($('setupProbeState').textContent);}
 $('setupProbeState').textContent=axes==='xyz'?'Work zero is the front-left corner of the stock, at its surface. Remove the plate and clip before cutting.':'Work Z zero is the stock surface under the plate. Remove the plate and clip before cutting.';
 message($('setupProbeState').textContent);
});

// ---- settings ----
function showSettings(){
 const settings=latest?.settings||{},keys=Object.keys(settings).filter(k=>/^\d{1,3}$/.test(k)).sort((a,b)=>a-b),list=$('setupSettingsList');
 list.replaceChildren();
 if(!keys.length){list.textContent='No numbered settings were reported. Connect a Grbl-family controller first.';return;}
 for(const key of keys){
   const row=document.createElement('div');row.className='pair';row.dataset.setting=key;
   const label=document.createElement('label');label.textContent=`$${key}${SETTING_NAMES[key]?' · '+SETTING_NAMES[key]:''}`;
   const input=document.createElement('input');input.type='number';input.step='any';input.value=settings[key];input.setAttribute('aria-label',`Setting ${key}`);
   const button=document.createElement('button');button.type='button';button.textContent='Write';
   button.onclick=task(()=>writeSetting(key,input.value));
   label.append(input);row.append(label,button);list.append(row);
 }
}
async function writeSetting(key,value){
 const next=Number(value);
 if(value===''||!Number.isFinite(next))throw Error('Enter a number for the setting');
 if(!confirm(`Write $${key}=${next} to the controller? Its present value is ${latest?.settings?.[key]}.`))return false;
 await api('setup/setting',{confirmed:true,key,value:next});
 const status=await settle();latest=status;
 if(status.error)throw Error(`The controller refused the setting: ${status.error}`);
 showSettings();syncCalibration();
 message(`$${key} is now ${status.settings?.[key]}`);
 return true;
}
$('setupSettingsRefresh').onclick=task(()=>showSettings());
$('setupSettingsExport').onclick=task(()=>{
 const settings=latest?.settings||{};
 if(!Object.keys(settings).length)throw Error('There are no settings to export; connect the controller first');
 download('controller-settings.json',JSON.stringify({format:'kerfkite-controller-settings/1',protocol:latest.protocol,identification:latest.identification||[],settings},null,1),'application/json');
});

// ---- calibration ----
function syncCalibration(){const key=STEPS[$('setupCalAxis').value],steps=latest?.settings?.[key];if(steps!==undefined)$('setupCalSteps').value=steps;showCalibration();}
function showCalibration(){
 try{const r=calibrate($('setupCalSteps').value,$('setupCalCommanded').value,$('setupCalMeasured').value);$('setupCalResult').textContent=`Corrected: ${r.steps} steps per mm (${r.change>=0?'+':''}${r.change}%)`;return r;}
 catch(e){$('setupCalResult').textContent=e.message;return null;}
}
for(const id of ['setupCalSteps','setupCalCommanded','setupCalMeasured'])$(id).addEventListener('input',showCalibration);
$('setupCalAxis').onchange=syncCalibration;
$('setupCalibrate').addEventListener('toggle',()=>{if($('setupCalibrate').open)syncCalibration();});
$('setupCalWrite').onclick=task(async()=>{
 const r=calibrate($('setupCalSteps').value,$('setupCalCommanded').value,$('setupCalMeasured').value),axis=$('setupCalAxis').value;
 if(await writeSetting(STEPS[axis],r.steps)){$('setupCalMeasured').value=$('setupCalCommanded').value;message(`${axis} now has ${r.steps} steps per mm. Repeat the move to confirm it.`);}
});

// ---- surfacing ----
const syncArea=()=>{$('setupSurfaceWidth').value=$('width').value;$('setupSurfaceHeight').value=$('height').value;};
syncArea();root.addEventListener('cnc-project-loaded',syncArea);
$('setupSurfaceAdd').onclick=task(()=>{
 const width=Number($('setupSurfaceWidth').value),height=Number($('setupSurfaceHeight').value),tool=Number($('setupSurfaceTool').value),depth=number($('setupSurfaceDepth').value,'Depth to remove',.05,5);
 if(width>Number($('width').value)||height>Number($('height').value))throw Error('The area is larger than the work area');
 const made=surfacing(width,height,tool,Number($('setupSurfaceStep').value));
 const settings=editorSettings();
 objects=[...objects,{designLayerId:window.CNCDesignLayers?.activeId?.()||'default',name:'Spoilboard surfacing',paths:[made.path],operationSettings:{...settings,mode:'router',operation:'line',tool_diameter:tool,depth,step:depth}}];
 selected=objects.length-1;invalidate();draw();
 message(`Surfacing job added: ${made.passes} passes ${made.pitch} mm apart, ${depth} mm deep. It uses the CAM feed and spindle speed; preview it before running.`);
});

// ---- macros ----
let key='kerfkite-sender-macros',library=starterMacros();
const store=()=>{try{localStorage.setItem(key,JSON.stringify(library));}catch{message('The macros could not be saved in this browser; export them to keep them.',true);}};
function showMacros(name){
 const list=$('setupMacroList');list.replaceChildren(...library.macros.map(m=>new Option(m.name,m.name)));
 if(name)list.value=name;
 const current=library.macros[list.selectedIndex];
 $('setupMacroName').value=current?.name||'';$('setupMacroText').value=current?.text||'';
}
$('setupMacroList').onchange=()=>showMacros($('setupMacroList').value);
$('setupMacroSave').onclick=task(()=>{
 const made=macro({name:$('setupMacroName').value,text:$('setupMacroText').value}),at=library.macros.findIndex(m=>m.name.toLowerCase()===made.name.toLowerCase());
 if(at<0&&library.macros.length>=100)throw Error('A macro library holds at most 100 macros');
 if(at<0)library.macros.push(made);else library.macros[at]=made;
 store();showMacros(made.name);message(`Macro ${made.name} saved`);
});
$('setupMacroDelete').onclick=task(()=>{
 const current=library.macros[$('setupMacroList').selectedIndex];
 if(!current||!confirm(`Delete the macro ${current.name}?`))return;
 library.macros=library.macros.filter(m=>m!==current);store();showMacros();
});
$('setupMacroRun').onclick=task(async()=>{
 const lines=macroLines($('setupMacroText').value);
 if(!confirm(`Run ${lines.length} line${lines.length>1?'s':''} on the connected controller?\n\n${lines.slice(0,12).join('\n')}${lines.length>12?'\n…':''}`))return;
 await api('setup/macro',{confirmed:true,lines});
 const status=await settle();
 if(status.error)throw Error(`The macro stopped: ${status.error}`);
 message(`Macro finished: ${lines.length} line${lines.length>1?'s':''} sent`);
});
$('setupMacroExport').onclick=task(()=>download('kerfkite-macros.json',JSON.stringify(library,null,1),'application/json'));
$('setupMacroImport').onclick=()=>$('setupMacroFile').click();
$('setupMacroFile').onchange=task(async()=>{
 const file=$('setupMacroFile').files[0];$('setupMacroFile').value='';if(!file)return;
 if(file.size>500000)throw Error('The macro file is too large');
 library=macroLibrary(JSON.parse(await file.text()));store();showMacros();message(`${library.macros.length} macros imported`);
});

const features=async()=>{try{const f=await api('setup');box.hidden=false;$('setupProbe').hidden=!f.touch_plate;$('setupSettings').hidden=$('setupCalibrate').hidden=!f.settings;}catch{}};
$('protocol')?.addEventListener('change',()=>setTimeout(features,800));  // after the server has taken the new protocol
root.studioReady?.then(async session=>{
 if((typeof webMode!=='undefined'&&webMode)||session?.offline)return;
 if(session?.username)key+=':'+session.username;
 try{const text=localStorage.getItem(key);if(text)library=macroLibrary(JSON.parse(text));}catch{library=starterMacros();}
 showMacros();
 // A server without the setup routes leaves the panel hidden.
 await features();
});
})(typeof window!=='undefined'?window:globalThis);
