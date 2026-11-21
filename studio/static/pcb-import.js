// Circuit board import: a copper layer, a board outline and a drill file become reviewable layers of the
// design, one for each tool: isolation contours, each drill diameter, slots and the cut-out. The server reads
// the files and computes the geometry (/api/pcb-import); nothing here talks to a machine. See docs/PCB.md.
(()=>{
'use strict';
const host=$('design-panel-projects');
if(!host||offline)return;
const Pcb=window.KerfKitePcb;
const panel=document.createElement('section');
panel.className='solid-group';panel.id='pcb-panel';panel.hidden=true;
panel.innerHTML=`<h3>Circuit board</h3>
<p class="hint">Engrave a board from the files a design program exports for manufacture. Review shows what was read and every warning; Add to design makes one layer for each tool, with Output off. Importing sends nothing to a machine.</p>
<label>Copper layer · Gerber<input id="pcb-copper" type="file"></label>
<label>Board outline · Gerber<input id="pcb-outline" type="file"></label>
<label>Drill file · Excellon<input id="pcb-drill" type="file"></label>
<label>Side<select id="pcb-side"><option value="top">Top</option><option value="bottom">Underside, mirrored</option></select></label>
<div class="pair"><label>Board corner X · mm<input id="pcb-corner-x" type="number" value="5" min="0" max="1000" step=".5"></label>
<label>Board corner Y · mm<input id="pcb-corner-y" type="number" value="5" min="0" max="1000" step=".5"></label></div>
<label class="check" id="pcb-register-label" hidden><input type="checkbox" id="pcb-register" checked> Register with the board already in this design</label>
<details open><summary>Engraving tool</summary>
<label>Tool<select id="pcb-kind"><option value="v">V-bit</option><option value="width">Width of cut stated</option></select></label>
<div class="pair" id="pcb-vbit"><label>Tip width · mm<input id="pcb-tip" type="number" value="0.1" min="0" max="10" step=".01"></label><label>Included angle · °<input id="pcb-angle" type="number" value="30" min="10" max="170" step="1"></label></div>
<div class="pair"><label>Engraving depth · mm<input id="pcb-depth" type="number" value="0.1" min="0.01" max="10" step=".01"></label><label>Width of cut · mm<input id="pcb-width" type="number" value="0.2" min="0.01" max="10" step=".01"></label></div>
<div class="pair"><label>Passes<input id="pcb-passes" type="number" value="1" min="1" max="50" step="1"></label><label>Overlap · %<input id="pcb-overlap" type="number" value="20" min="0" max="90" step="5"></label></div>
<div class="pair"><label>Direction<select id="pcb-direction"><option value="climb">Climb</option><option value="conventional">Conventional</option></select></label><label>Arc tolerance · mm<input id="pcb-arc" type="number" value="0.005" min="0.0005" max="0.1" step=".001"></label></div>
<label class="check"><input type="checkbox" id="pcb-clip"> Cut the isolation off at the board outline</label>
</details>
<details><summary>Drilling and cut-out</summary>
<div class="pair"><label>Drill depth · mm<input id="pcb-drill-depth" type="number" value="1.8" min="0.01" max="50" step=".1"></label><label>Cut-out cutter · mm<input id="pcb-cutter" type="number" value="1.5" min="0.01" max="100" step=".1"></label></div>
<div class="pair"><label>Cut-out depth · mm<input id="pcb-cut-depth" type="number" value="1.8" min="0.01" max="50" step=".1"></label><label>Cut-out step down · mm<input id="pcb-cut-step" type="number" value="0.6" min="0.01" max="10" step=".1"></label></div>
</details>
<details id="pcb-format"><summary>Drill number format</summary>
<p class="hint" id="pcb-format-read">Many drill files leave out part of their number format. After a review this shows how the file was read; state the format here if the holes land in the wrong places.</p>
<div class="pair"><label>Units<select id="pcb-units"><option value="">As the file states</option><option value="mm">Millimetres</option><option value="inch">Inches</option></select></label>
<label>Zeros omitted<select id="pcb-zeros"><option value="">As the file states</option><option value="leading">Leading</option><option value="trailing">Trailing</option></select></label>
<label>Integer digits<input id="pcb-integer" type="number" min="1" max="6" step="1" placeholder="file"></label><label>Decimal digits<input id="pcb-decimal" type="number" min="0" max="6" step="1" placeholder="file"></label></div>
</details>
<div class="buttons"><button id="pcb-review">Review board</button></div>
<div id="pcb-result" hidden><ul id="pcb-summary" class="hint pcb-list"></ul><h4 id="pcb-warning-title">Warnings</h4><ul id="pcb-warnings" class="hint pcb-list"></ul>
<button id="pcb-commit">Add to design</button></div>
<label class="check"><input type="checkbox" id="pcb-show" checked> Show the copper and the places the cutter cannot reach</label>
<p id="pcb-status" role="status"></p>
<div id="pcb-stages" hidden><h4>Board stages</h4><p class="hint">Each stage is one tool. Choose a stage, preview it, change the tool and set work Z, then run it; or turn every stage on and use Manual tool changes in CAM to export one file for each tool.</p><div id="pcb-stage-list"></div><div class="buttons"><button id="pcb-all-stages">Output every stage</button><button id="pcb-forget">Forget board marks</button></div></div>`;
const importPanel=host.querySelector('.solid-group');
if(importPanel)importPanel.after(panel);else host.prepend(panel);

// The copper of the board under review lies beneath the drawing; the marks lie above everything.
const copperLayer=document.createElementNS(NS,'g'),markLayer=document.createElementNS(NS,'g');
copperLayer.id='pcbCopper';markLayer.id='pcbMarks';
for(const layer of [copperLayer,markLayer])layer.style.pointerEvents='none';
$('toolpaths').before(copperLayer);$('canvas').append(markLayer);

let draft=null,revision=0;
const status=textValue=>{$('pcb-status').textContent=textValue;};
const boards=()=>Array.isArray(window.projectExtras?.pcbBoards)?window.projectExtras.pcbBoards:[];
const value=id=>$(id).value===''?null:$(id).valueAsNumber;
function items(list,lines){list.replaceChildren(...lines.map(line=>{const li=document.createElement('li');li.textContent=line;return li;}));}

function showWidth(){
  const v=$('pcb-kind').value==='v';
  $('pcb-vbit').hidden=!v;$('pcb-width').readOnly=v;
  if(!v)return;
  try{$('pcb-width').value=Pcb.vBitWidth(value('pcb-tip'),value('pcb-angle'),value('pcb-depth'));}catch{$('pcb-width').value='';}
}

function paint(){
  const h=Number($('height').value),shown=$('pcb-show').checked;
  copperLayer.replaceChildren();markLayer.replaceChildren();
  if(!shown)return;
  const copper=draft?.answer.copper;
  if(copper){
    const ring=points=>'M'+points.map(([x,y])=>x+','+(h-y)).join(' L')+' Z';
    const node=svgNode('path',{d:copper.copper.map(island=>ring(island.shell)+island.holes.map(ring).join('')).join(''),fill:'#c9772b','fill-opacity':'.4','fill-rule':'evenodd',stroke:'none'});
    copperLayer.append(node);
  }
  // The outline and the holes of the board under review, so that a misplaced layer is seen before it is added.
  const outline=draft?.answer.outline,drill=draft?.answer.drill;
  if(outline)copperLayer.append(svgNode('path',{d:drawingPath(outline.paths,h),fill:'none',stroke:'#34495e','stroke-width':'.3'}));
  for(const operation of drill?.operations||[])for(const [[x,y]] of operation.paths)
    copperLayer.append(svgNode('circle',{cx:x,cy:h-y,r:operation.tool_diameter/2,fill:'#fff',stroke:'#1f7a4d','stroke-width':'.15',class:'pcb-hole'}));
  const places=[...(copper?.unreachable||[]),...boards().flatMap(board=>board.unreachable||[])];
  for(const place of places){
    const mark=svgNode('circle',{cx:place.at[0],cy:h-place.at[1],r:Math.max(.6,(place.gap||0)*1.5),fill:'none',stroke:'#c62828','stroke-width':'.3',class:'pcb-unreachable'});
    const title=document.createElementNS(NS,'title');
    title.textContent=place.gap==null?'The cutter cannot pass here':'The cutter cannot pass here: the gap is '+place.gap.toFixed(3)+' mm';
    mark.append(title);markLayer.append(mark);
  }
}
const previousDraw=draw;draw=function(){previousDraw();paint();};

function clear(){revision++;draft=null;$('pcb-result').hidden=true;status('');paint();}

function showStages(){
  const list=boards(),rows=CNCDesignLayers.layers(),stageIds=list.flatMap(board=>board.layers).filter(id=>rows.some(row=>row.id===id));
  $('pcb-stages').hidden=!stageIds.length&&!list.length;
  $('pcb-register-label').hidden=!list.length;
  for(const id of ['pcb-corner-x','pcb-corner-y'])$(id).disabled=list.length>0&&$('pcb-register').checked;
  $('pcb-stage-list').replaceChildren(...stageIds.map(id=>{
    const row=rows.find(r=>r.id===id),line=document.createElement('div'),button=document.createElement('button');
    line.className='buttons';
    button.textContent=row.name+(row.output?' · output on':'');button.setAttribute('aria-pressed',String(row.output));button.dataset.stage=id;
    button.onclick=task(()=>{
      // One tool at a time: this stage on, the other board stages off. Other layers are left as they are.
      const next=CNCDesignLayers.layers();
      for(const r of next)if(stageIds.includes(r.id))r.output=r.id===id;
      apply(next);
      message(row.name+' is the job. Generate a preview; fit this stage\'s tool and set work Z before running it.');
    });
    line.append(button);return line;
  }));
}
function apply(rows){
  const valid=CNCDesignLayers.catalog(rows);
  invalidate();window.projectExtras={...window.projectExtras,designLayers:valid};draw();
  window.dispatchEvent(new Event('cnc-layers-changed'));window.dispatchEvent(new Event('cnc-project-changed'));showStages();
}

async function fileText(id){
  const file=$(id).files[0];
  if(!file)return '';
  if(file.size>4*1024*1024)throw Error('Board files up to 4 MiB can be imported');
  // Bytes that are not UTF-8 can only be in comments.
  return new TextDecoder('utf-8').decode(await file.arrayBuffer());
}
function values(){
  const registered=boards()[0]&&$('pcb-register').checked?boards()[0]:null;
  return {side:$('pcb-side').value,cornerX:value('pcb-corner-x'),cornerY:value('pcb-corner-y'),width:Number($('width').value),height:Number($('height').value),registered,
    kind:$('pcb-kind').value,tip:value('pcb-tip'),angle:value('pcb-angle'),depth:value('pcb-depth'),toolWidth:value('pcb-width'),
    passes:value('pcb-passes'),overlapPercent:value('pcb-overlap'),arcTolerance:value('pcb-arc'),direction:$('pcb-direction').value,clip:$('pcb-clip').checked,
    units:$('pcb-units').value,zeros:$('pcb-zeros').value,integer:value('pcb-integer'),decimal:value('pcb-decimal'),
    drillDepth:value('pcb-drill-depth'),cutter:value('pcb-cutter'),cutDepth:value('pcb-cut-depth'),cutStep:value('pcb-cut-step'),
    peck:Number($('drill_peck').value)||0,pointAngle:Number($('drill_point_angle').value)||118};
}
async function review(){
  clear();
  const id=revision,settings=values();
  const files={copper:await fileText('pcb-copper'),outline:await fileText('pcb-outline'),drill:await fileText('pcb-drill')};
  const body=Pcb.request(files,settings);
  status('Reading the board…');
  let answer;
  try{answer=await api('pcb-import',body);}
  catch(error){
    status('');
    // A drill file that cannot be read without its format is corrected in the format fields.
    if(/drill|units|digits|zeros/i.test(error.message))$('pcb-format').open=true;
    throw error;
  }
  if(id!==revision)return;
  const firstTool=1+Math.max(0,...objects.map(o=>Number(o.operationSettings?.tool_id)||0));
  const staged=Pcb.plan(answer,{...settings,firstTool});
  draft={answer,staged};
  items($('pcb-summary'),Pcb.summary(answer));
  const notes=Pcb.warnings(answer);
  $('pcb-warning-title').hidden=!notes.length;items($('pcb-warnings'),notes);
  if(answer.drill){
    $('pcb-format-read').textContent='Read as '+Pcb.formatText(answer.drill.interpretation)+'. If the holes are in the wrong places, state the format here and review again.';
    if(Pcb.formatGuessed(answer.drill.interpretation)||answer.drill.warnings.some(w=>/request/.test(w)))$('pcb-format').open=true;
  }
  $('pcb-result').hidden=false;
  const places=answer.copper?.unreachable_count||0;
  status('Review ready: '+staged.objects.length+' objects for '+staged.tools+(staged.tools===1?' tool':' tools')+
    (places?'. The cutter cannot reach '+places+(places===1?' place':' places')+', marked in red.':'.'));
  paint();
}
function commit(){
  const d=draft;if(!d)throw Error('Review the board first');
  if($('mode').value!=='router'){
    if(objects.length)throw Error('This design is set up for another head; a circuit board is cut with the router. Start a new project or change the head first.');
    $('mode').value='router';
  }
  const rows=CNCDesignLayers.layers(),map=new Map();
  if(rows.length+d.staged.layers.length>64)throw Error('Adding these layers would exceed the 64-layer limit. Open a new project first.');
  for(const layer of d.staged.layers){
    const id='pcb_'+crypto.randomUUID().replaceAll('-','');map.set(layer.key,id);
    // Output starts off, as for every import: nothing cuts until the stage is reviewed and chosen.
    rows.push({id,name:layer.name.slice(0,80),color:layer.color,visible:true,output:false,settings:{}});
  }
  const valid=CNCDesignLayers.catalog(rows);
  const additions=d.staged.objects.map(({sourceLayer,...data})=>({...data,designLayerId:map.get(sourceLayer)}));
  const combined=objects.concat(additions),paths=combined.flatMap(o=>o.paths),points=paths.reduce((n,p)=>n+p.length,0);
  if(combined.length>1000||paths.length>3000||points>100000)
    throw Error('The board has '+d.staged.paths+' paths and '+d.staged.points+' points, more than a Studio project holds (3000 paths, 100000 points). Use fewer passes or a larger arc tolerance.');
  if(paths.some(p=>p.some(pt=>pt.some(v=>!Number.isFinite(v)||Math.abs(v)>2000))))throw Error('The board lies outside the range of a Studio project');
  const records=[...boards(),Pcb.record(d.answer,[...map.values()])];
  if(records.length>8)throw Error('A project keeps up to eight board imports');
  const extra={...window.projectExtras,designLayers:valid,pcbBoards:records};
  if(new TextEncoder().encode(JSON.stringify({...CNCProjects.document(),objects:combined,extras:extra},null,2)).length>4e6)throw Error('The board exceeds the portable project size limit');
  window.projectExtras=extra;objects=combined;selected=combined.length-1;
  const count=additions.length,stages=d.staged.layers.length;
  draft=null;revision++;$('pcb-result').hidden=true;
  modeUI();invalidate();draw();window.dispatchEvent(new Event('cnc-layers-changed'));window.dispatchEvent(new Event('cnc-project-changed'));
  showStages();
  status('Added '+count+' objects in '+stages+(stages===1?' stage':' stages')+'. Output is off for each: choose a stage below to make it the job.');
}
$('pcb-review').onclick=task(review);
$('pcb-commit').onclick=task(commit);
$('pcb-all-stages').onclick=task(()=>{
  const ids=boards().flatMap(board=>board.layers),next=CNCDesignLayers.layers();
  for(const row of next)if(ids.includes(row.id))row.output=true;
  apply(next);message('Every board stage is on. Use Manual tool changes in CAM to export one file for each tool.');
});
$('pcb-forget').onclick=task(()=>{
  const {pcbBoards,...rest}=window.projectExtras||{};
  window.projectExtras=rest;window.dispatchEvent(new Event('cnc-project-changed'));showStages();paint();
  message('Board marks forgotten. The layers and objects stay in the design.');
});
for(const id of ['pcb-kind','pcb-tip','pcb-angle','pcb-depth'])$(id).addEventListener('input',showWidth);
for(const node of panel.querySelectorAll('input:not(#pcb-show),select'))node.addEventListener('input',()=>{if(draft)clear();});
$('pcb-register').addEventListener('input',showStages);
$('pcb-show').addEventListener('input',paint);
window.addEventListener('cnc-project-loaded',()=>{clear();showStages();});
window.addEventListener('cnc-layers-changed',showStages);
showWidth();
window.studioReady?.then(()=>{if(window.studioSession?.pcb_import){panel.hidden=false;showStages();paint();}});
})();
