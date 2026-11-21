// Probe levelling: measure the stock surface with the controller's probe, then run the previewed
// router job so that every cut follows that surface. For thin engraving, such as isolation routing
// on a circuit board, where the depth has to hold to a few hundredths of a millimetre.
(()=>{
const panel=$('machinePanel'),anchor=$('confirmed')?.closest('label');
if(!panel||!anchor||offline)return;
const box=document.createElement('details');
box.id='levelPanel';box.hidden=true;
box.innerHTML=`<summary>Probe levelling</summary>
<p class="hint">Probes the stock on a grid over the previewed router job and follows that surface while cutting. Needs a Grbl-family controller with a probe input. Connect the probe, test that it triggers, stop the spindle and set work zero just above or on the stock before probing. Work Z zero is set where the first point touches.</p>
<div class="pair"><label>Probe spacing · mm<input id="levelSpacing" type="number" value="10" min="1" max="100" step="1"></label><label>Probe feed · mm/min<input id="levelFeed" type="number" value="25" min="1" max="200" step="1"></label></div>
<div class="pair"><label>Probe clearance Z · mm<input id="levelClearance" type="number" value="2" min=".5" max="25" step=".5"></label><label>Probe depth · mm<input id="levelDepth" type="number" value="1" min=".1" max="5" step=".1"></label></div>
<label class="check"><input type="checkbox" id="levelConfirmed"> Probe connected and tested, spindle stopped, work zero set at the stock surface</label>
<div class="buttons"><button id="levelProbe">Probe surface</button><button id="levelClear">Forget surface</button><button id="levelExport">Export levelled G-code</button></div>
<div id="levelState">No surface probed</div>
<label class="check"><input type="checkbox" id="levelUse" disabled> Follow the probed surface when running</label>`;
panel.insertBefore(box,anchor);
let busy=false;
const millimetres=value=>(value>0?'+':'')+value.toFixed(3);
function show(state){
  const surface=state.surface,plan=state.plan;
  if(surface){
    $('levelState').textContent=`Surface probed at ${surface.offsets.length} points (${surface.columns} × ${surface.rows}): ${millimetres(surface.lowest)} to ${millimetres(surface.highest)} mm from work zero`;
    $('levelUse').disabled=false;
  }else{
    $('levelState').textContent=plan?`Probing: ${plan.probed} of ${plan.points.length} points`:'No surface probed';
    $('levelUse').disabled=true;$('levelUse').checked=false;
  }
}
async function refresh(){show(await api('level'));}
const current=()=>{
  if(!preview||previewProject!==JSON.stringify(machiningProject()))throw Error('Generate a current preview first');
  return machiningProject();
};
$('levelProbe').onclick=task(async()=>{
  if(busy)throw Error('Probing is already running');
  const project=current();
  if(!$('levelConfirmed').checked)throw Error('Confirm the probe setup first');
  const number=id=>Number($(id).value);
  busy=true;
  try{
    show(await api('level/probe',{project,confirmed:true,spacing:number('levelSpacing'),feed:number('levelFeed'),clearance:number('levelClearance'),depth:number('levelDepth')}));
    $('levelConfirmed').checked=false;
    message('Probing started; keep clear of the machine');
    for(;;){
      await new Promise(resolve=>setTimeout(resolve,400));
      const [status,state]=[await api('status'),await api('level')];
      if(!state.plan)throw Error('Probing was cancelled');
      show(state);
      if(!status.active)break;
    }
    show(await api('level/collect',{}));
    $('levelUse').checked=true;
    message('Surface probed. The next run follows it; untick to run without levelling.');
  }finally{busy=false;}
});
$('levelClear').onclick=task(async()=>{show(await api('level/clear',{}));message('Probed surface forgotten');});
$('levelExport').onclick=task(async()=>{
  const result=await api('level/program',{project:current()});
  download('engraving-levelled.nc',result.lines.join('\n')+'\n');
});
// Setting zero, homing, changing the head or reconnecting forgets the surface on the server.
setInterval(()=>{if(box.open&&!box.hidden&&!busy)refresh().catch(()=>{});},2000);
window.KerfKiteLeveling={
  // Extra fields for the run request, read when the job is submitted.
  runOptions:()=>$('levelUse').checked?{leveled:true}:{},
  // A phone start sends the previewed program as it is, so it cannot follow the surface.
  requireWorkstationStart(){if($('levelUse').checked)throw Error('A levelled job starts from this workstation: use Run previewed job, or untick Follow the probed surface');},
};
window.studioReady?.then(()=>{if(window.studioSession?.probe_leveling&&!webMode){box.hidden=false;refresh().catch(()=>{});}});
})();
