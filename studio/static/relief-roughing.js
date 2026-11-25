/* Relief roughing: passes that take the stock down level by level before the finishing pass of a 3D relief,
   along the rasters or by adaptive clearing.
   The request is the relief's own, with the roughing cutter, an allowance and a step down; the server works
   out the passes. They are added as one object with its own operation, ahead of the finishing relief. */
(()=>{
'use strict';
const make=document.getElementById('makeRelief');
if(!make||(typeof offline!=='undefined'&&offline))return;
const box=document.createElement('div');box.id='reliefRoughing';
box.innerHTML=`<h3>Roughing first</h3>
<p class="hint">A deep relief is roughed out before it is finished: a flat cutter takes the stock down one level at a time and leaves an allowance, and the finishing cutter then takes a light, even cut. Choose the same file and placement as the relief, then add the roughing passes. They are placed ahead of the finishing relief in the cutting order.</p>
<label>Roughing strategy<select id="roughStrategy"><option value="raster">Level passes along the rasters · follows the surface closely</option><option value="adaptive">Adaptive clearing · keeps the width of cut even</option></select></label>
<div class="pair"><label>Roughing cutter diameter · mm<input id="roughTool" type="number" min=".1" max="50" step=".001" value="6.35"></label><label>Step down for each level · mm<input id="roughStep" type="number" min=".05" max="100" step=".05" value="2"></label><label>Allowance left for finishing · mm<input id="roughAllowance" type="number" min="0" max="10" step=".05" value=".5"></label><label><span id="roughStepoverLabel">Stepover · mm</span><input id="roughStepover" type="number" min=".1" max="50" step=".1" value="2.5"></label></div>
<button id="makeRoughing" type="button">Add roughing passes</button>`;
make.closest('details').append(box);
$('roughStrategy').onchange=()=>{$('roughStepoverLabel').textContent=$('roughStrategy').value==='adaptive'?'Width of cut to keep · mm':'Stepover · mm';};
$('makeRoughing').onclick=task(async()=>{
 const {file,data}=await window.CNCReliefRequest();
 const adaptive=$('roughStrategy').value==='adaptive';
 const tool=Number($('roughTool').value),stepover=Number($('roughStepover').value),allowance=Number($('roughAllowance').value),step=Number($('roughStep').value);
 if(!(tool>=.1&&tool<=50))throw Error('Roughing cutter diameter must be between 0.1 and 50 mm');
 if(!(stepover>0&&stepover<=tool))throw Error('The roughing stepover must be more than 0 and no more than the cutter diameter');
 if(adaptive&&stepover<.08*tool)throw Error('For adaptive clearing the width of cut must be at least 8% of the cutter diameter');
 message('Calculating roughing passes…');
 const result=await api('relief-roughing',{...data,tool_shape:'flat',tool_diameter:tool,stepover,allowance,rough_step:step,...(adaptive?{rough_strategy:'adaptive'}:{})});
 if($('reliefFile').files[0]!==file)throw Error('The relief source changed. Add the roughing passes again.');
 const [bx,by,mx,my]=bounds(result.paths);
 if(bx<0||by<0||mx>Number($('width').value)||my>Number($('height').value))throw Error('The roughing passes exceed the work area.');
 const settings=editorSettings();
 // The passes carry their own levels, so the job takes each in one go: its step down is the whole depth.
 // Roughing and finishing use different cutters, so each is a stage of its own: both carry a tool identity
 // for Manual tool changes. A finishing relief that already has one keeps it.
 const finishing=objects.findIndex(o=>o.name==='Relief · '+file.name);
 const taken=new Set(objects.map(o=>o.operationSettings?.tool_id).filter(Number.isInteger));
 const free=()=>{let id=1;while(taken.has(id))id++;taken.add(id);return id;};
 const object={designLayerId:window.CNCDesignLayers?.activeId?.()||'default',name:(adaptive?'Relief roughing (adaptive) · ':'Relief roughing · ')+file.name,paths:result.paths,depths:result.path_depths,
   operationSettings:{...settings,mode:'router',operation:'line',tool_diameter:tool,stepover,depth:Number((-result.lowest).toFixed(3)),step:Number((-result.lowest).toFixed(3)),tool_id:free(),tool_name:`Roughing · ${tool} mm flat`,tool_shape:'flat'}};
 const next=objects.slice();
 if(finishing>=0&&!Number.isInteger(next[finishing].operationSettings?.tool_id)){
   const shape=next[finishing].surface?.tool_shape==='flat'?'flat':'ball',diameter=next[finishing].surface?.tool_diameter||settings.tool_diameter;
   next[finishing]={...next[finishing],operationSettings:{...settings,mode:'router',operation:'line',...next[finishing].operationSettings,tool_diameter:diameter,tool_id:free(),tool_name:`Finishing · ${diameter} mm ${shape}`,tool_shape:shape}};
 }
 // Ahead of the finishing relief of the same file, when there is one; otherwise last.
 next.splice(finishing<0?next.length:finishing,0,object);
 objects=next;selected=finishing<0?objects.length-1:finishing;
 $('mode').value='router';invalidate();modeUI();draw(); const how=adaptive?`adaptive clearing keeping about ${stepover} mm of cut, ${result.helix_entries} helix ${result.helix_entries===1?'entry':'entries'}${result.plunge_entries?` and ${result.plunge_entries} straight down (use a centre-cutting cutter)`:''}${result.forced_length>0?`, ${result.forced_length.toFixed(0)} mm at full width in narrow channels`:''}; walls keep up to ${result.wall_extra.toFixed(2)} mm more than the allowance and slopes are left in steps`:`${result.paths.length} passes`;
 message(`Roughing added: ${result.levels} levels of ${result.step} mm down to ${(-result.lowest).toFixed(2)} mm, ${how}, leaving ${allowance} mm. It uses a different cutter from the finishing pass: preview one with the other's output off, or use Manual tool changes for a stage file for each cutter.`);
});
})();
