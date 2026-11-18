'use strict';
(() => {
  const panel = document.createElement('details');
  panel.id = 'rotaryPanel';
  panel.innerHTML = `<summary>Rotary · cylindrical machining</summary>
    <label>Cylinder diameter · mm<input id="rotaryDiameter" type="number" min="1" max="2000" value="50" step="any"></label>
    <label>Design axis to wrap<select id="rotaryAxis"><option value="Y">Y → angle (X stays axial)</option><option value="X">X → angle (Y design becomes axial)</option></select></label>
    <label>Start angle · degrees<input id="rotaryAngle" type="number" min="-360000" max="360000" value="0" step="any"></label>
    <label>Direction<select id="rotaryDirection"><option value="1">Positive</option><option value="-1">Reverse</option></select></label>
    <label>Postprocessor<select id="rotaryPost"><option value="linuxcnc">LinuxCNC · router export</option><option value="grblhal">grblHAL · rotary router or laser</option><option value="xpro-v5">xPRO V5 · independent A (XYAZ firmware)</option></select></label>
    <label>Physical rotary axis<select id="rotaryOutput"><option value="A">A around X · radial Z</option><option value="B">B around Y · radial Z</option><option value="C">C around Z · radial X</option></select></label>
    <label>Feed reference<select id="rotaryFeed"><option value="surface">Nominal stock surface</option><option value="cut">At cutting radius</option></select></label>
    <label><input id="rotaryFixture" type="checkbox"> Check fixture clearance envelope</label>
    <div id="rotaryLimits" hidden><label>Axial minimum · mm<input id="rotaryMin" type="number" value="0" step="any"></label><label>Axial maximum · mm<input id="rotaryMax" type="number" value="300" step="any"></label><label>Minimum tool-tip radius · mm<input id="rotaryRadius" type="number" min="0" value="0" step="any"></label></div>
    <p id="rotarySize" class="hint"></p><button id="rotaryWorkspace">Set design span to one revolution</button>
    <label>Periodic border cycles<input id="rotaryCycles" type="number" value="6" min="1" max="60"></label>
    <div class="buttons"><button id="rotaryBorder">Add seamless periodic border</button><button id="rotarySeam">Check seam endpoints</button></div>
    <button id="rotaryPreview">Preview wrapped job</button>
    <div id="rotaryResult" hidden><canvas id="rotaryCanvas" width="560" height="250" style="width:100%;height:auto;display:block" aria-label="Wrapped toolpath simulation"></canvas>
    <label>Inspect around cylinder · degrees<input id="rotaryOrbit" type="range" min="-180" max="180" value="0"></label>
    <label>Simulated cuts · percent<input id="rotaryProgress" type="range" min="0" max="100" step="0.1" value="100"></label>
    <button id="rotaryPlay">Play simulation</button><p id="rotaryMetrics"></p><details><summary>Posted rotary instructions</summary><pre id="rotaryCode" style="max-height:16rem;overflow:auto"></pre></details></div>
    <button id="rotaryExport" disabled>Export rotary G-code</button>
    <label><input id="rotaryConfirmed" type="checkbox" disabled> I verified the independent rotary drive, degrees calibration, tool, work zero and fixture for this program</label>
    <button id="rotaryRun" disabled>Stream reviewed rotary job</button><p id="rotaryAvailability" class="hint"></p>
    <p id="rotaryStatus" role="status" aria-live="polite"></p><a href="./help.html#rotary" target="_blank" rel="noopener">Rotary setup and tutorial</a>`;
  document.querySelector('aside').append(panel);

  let reviewed = null, revision = 0, animation = 0, busy = false;
  const setup = ['rotaryDiameter','rotaryAngle','rotaryDirection','rotaryAxis','rotaryPost','rotaryOutput','rotaryFeed','rotaryFixture','rotaryMin','rotaryMax','rotaryRadius'];
  const report = (text, error=false) => { $('rotaryStatus').textContent=text; $('rotaryStatus').classList.toggle('error',error); };
  function options() {
    const value = id => { const n=$(id).valueAsNumber; if (!Number.isFinite(n) || !$(id).validity.valid) throw Error('Check '+$(id).closest('label').textContent.trim()+'.'); return n; };
    const result = {diameter:value('rotaryDiameter'), wrap_axis:$('rotaryAxis').value, angle_offset:value('rotaryAngle'), direction:Number($('rotaryDirection').value), postprocessor:$('rotaryPost').value, output_axis:$('rotaryOutput').value, feed_reference:$('rotaryFeed').value};
    if ($('rotaryFixture').checked) {
      result.fixture = {axial_min:value('rotaryMin'),axial_max:value('rotaryMax'),min_radius:value('rotaryRadius')};
      if (result.fixture.axial_min >= result.fixture.axial_max) throw Error('Fixture axial maximum must exceed its minimum.');
    }
    return result;
  }
  function enabled() {
    const target=reviewed?.options.postprocessor;
    let reason='Generate a current wrapped preview.';
    if (reviewed) {
      if (offline || webMode) reason='Export here. Streaming uses the machine workstation.';
      else if (!['grblhal','xpro-v5'].includes(target) || $('protocol').value!==target) reason='Select the matching grblHAL or xPRO V5 independent-A protocol and rotary postprocessor.';
      else if (!window.studioSession?.rotary_stream) reason='Update this workstation service to enable rotary streaming.';
      else if (!latest.connected || latest.protocol!==target) reason='Connect the matching simulator or commissioned rotary controller.';
      else if (!latest.rotary_axes?.includes(reviewed.options.output_axis)) reason='The connected controller has not reported the configured rotary axis.';
      else if (latest.state!=='Idle' || latest.active) reason='Wait for the connected controller to be idle.';
      else reason='';
    }
    $('rotaryConfirmed').disabled=Boolean(reason)||busy;
    if(reason) $('rotaryConfirmed').checked=false;
    $('rotaryRun').disabled=Boolean(reason)||busy||!$('rotaryConfirmed').checked;
    $('rotaryAvailability').textContent=reason || (latest.simulated?'Target: rotary simulator.':'Target: connected physical grblHAL controller.');
    $('rotaryExport').disabled=!reviewed||busy;
  }
  function stop() { cancelAnimationFrame(animation); animation=0; $('rotaryPlay').textContent='Play simulation'; }
  function clear() {
    revision++; stop(); reviewed=null; $('rotaryResult').hidden=true; $('rotaryConfirmed').checked=false;
    $('rotaryCode').textContent=''; $('rotaryMetrics').textContent=''; report(''); enabled();
  }
  const previousInvalidate=invalidate;
  invalidate=function(...args) { clear(); return previousInvalidate(...args); };
  function readSetup() {
    $('rotaryLimits').hidden=!$('rotaryFixture').checked;
    const o=options();
    $('rotarySize').textContent=`One revolution spans ${(Math.PI*o.diameter).toFixed(4)} mm of design.`;
    return o;
  }
  for(const id of setup) $(id).addEventListener('input',()=>{
    invalidate();
    try { window.projectExtras??={};window.projectExtras.rotary=readSetup();window.dispatchEvent(new Event('cnc-project-changed')); }
    catch(e) { report(e.message,true); }
  });
  function load() {
    clear(); const o=window.projectExtras?.rotary||{}, fixture=o.fixture||{};
    const values={rotaryDiameter:o.diameter??50,rotaryAxis:o.wrap_axis??'Y',rotaryAngle:o.angle_offset??0,rotaryDirection:o.direction??1,
      rotaryPost:o.postprocessor??'linuxcnc',rotaryOutput:o.output_axis??'A',rotaryFeed:o.feed_reference??'surface',rotaryMin:fixture.axial_min??0,rotaryMax:fixture.axial_max??300,rotaryRadius:fixture.min_radius??0};
    for(const [id,v] of Object.entries(values)) $(id).value=v;
    $('rotaryFixture').checked=Boolean(o.fixture);
    try { readSetup(); } catch(e) { report(e.message,true); }
  }
  window.addEventListener('cnc-project-loaded',load);
  const watchState=new MutationObserver(enabled);watchState.observe($('state'),{childList:true,characterData:true,subtree:true});
  $('rotaryConfirmed').oninput=enabled;
  function run(id, action) { $(id).onclick=async()=>{try{await action();}catch(e){report(e.message,true);}}; }
  run('rotaryWorkspace',()=>{
    const o=options(); $(o.wrap_axis==='X'?'width':'height').value=Math.PI*o.diameter; invalidate();draw();readSetup();report('The design span is one full revolution.');
  });
  run('rotaryBorder',()=>{
    const o=options(),c=Math.PI*o.diameter,n=$('rotaryCycles').valueAsNumber,wrapX=o.wrap_axis==='X',span=Number($(wrapX?'height':'width').value);
    if (!Number.isInteger(n)||n<1||n>60) throw Error('Use 1–60 whole cycles.');
    const count=n*64, path=Array.from({length:count+1},(_,i)=>{
      const t=i/count,axial=span/2+Math.min(10,span/4)*Math.sin(2*Math.PI*n*t);
      return wrapX?[c*t,axial]:[axial,c*t];
    });
    $(wrapX?'width':'height').value=c; add('Periodic rotary border · '+n+' cycles',[path]);
    report('Added one turn with matching position and tangent at the seam.');
  });
  run('rotarySeam',()=>{
    const o=options(),axis=o.wrap_axis==='X'?0:1,c=Math.PI*o.diameter,p=machiningProject(),edges=[[],[]];
    for(const operation of p.operations??[p]) {
      const layer={...p,...operation};
      layer.paths.forEach((path,j)=>{
        for(const i of [0,path.length-1]) for(const side of [0,1]) if(Math.abs(path[i][axis]-side*c)<=.01)
          edges[side].push([path[i][1-axis],layer.mode==='router'?(layer.path_depths?.[j]?.[i]??-layer.depth):0,layer.mode==='laser'?(layer.path_powers?.[j]?.[i]??layer.power):0]);
      });
    }
    for(const side of edges) side.sort((a,b)=>a[0]-b[0]||a[1]-b[1]||a[2]-b[2]);
    if(!edges[0].length||edges[0].length!==edges[1].length||edges[0].some((a,i)=>a.some((v,k)=>Math.abs(v-edges[1][i][k])>(k===2?.5:.01)))) throw Error('Seam endpoints differ in count, axial position, depth or laser power.');
    report(`${edges[0].length} seam endpoint pair(s) match in position and ${p.mode==='laser'?'power':'depth'}. Inspect tangent continuity in the preview.`);
  });
  function drawCylinder() {
    if(!reviewed) return;
    const canvas=$('rotaryCanvas'), ctx=canvas.getContext('2d'), {job,options:o}=reviewed;
    const radius=o.diameter/2, index=o.wrap_axis==='X'?0:1, angle=o.angle_offset+Number($('rotaryOrbit').value);
    const rad=Math.PI/180, scaleAngle=o.direction/radius;
    const lo=job.bounds[1-index], hi=job.bounds[3-index], w=canvas.width,h=canvas.height;
    const scale=Math.min((w-32)/(hi-lo+radius*.6||1),(h-26)/(radius*2||1));
    const pos=(axial,theta,r)=>[(w/2)+(axial-(lo+hi)/2+.28*r*Math.cos(theta))*scale, h/2-r*Math.sin(theta)*scale];
    ctx.fillStyle='#eff5f5';ctx.fillRect(0,0,w,h);ctx.strokeStyle='#799097';ctx.lineWidth=1;
    for(const axial of [lo,hi]) {
      ctx.beginPath();for(let i=0;i<=72;i++){const p=pos(axial,i*Math.PI/36,radius);if(i===0)ctx.moveTo(...p);else ctx.lineTo(...p);}ctx.stroke();
    }
    for(const theta of [-Math.PI/2,Math.PI/2]){ctx.beginPath();ctx.moveTo(...pos(lo,theta,radius));ctx.lineTo(...pos(hi,theta,radius));ctx.stroke();}
    ctx.setLineDash([4,4]);ctx.beginPath();ctx.moveTo(...pos(lo,angle*rad,radius));ctx.lineTo(...pos(hi,angle*rad,radius));ctx.stroke();ctx.setLineDash([]);
    const full=job.segments.length, end=Math.ceil(full*Number($('rotaryProgress').value)/100), stride=Math.max(1,Math.ceil(full/4500));
    const peakPower=Math.max(1,reviewed.maxPower);
    for(let j=0;j<end;j+=stride){
      const s=job.segments[j], a=s[index]*scaleAngle+angle*rad,b=s[index+2]*scaleAngle+angle*rad;
      const z0=s[6]??s[4],z1=s[4], start=s[1-index], finish=s[3-index];
      const parts=Math.max(1,Math.min(200,Math.ceil(Math.abs(b-a)/(Math.PI/22))));
      ctx.beginPath();ctx.moveTo(...pos(start,a,radius+z0));
      for(let k=1;k<=parts;k++) {const t=k/parts;ctx.lineTo(...pos(start+(finish-start)*t,a+(b-a)*t,radius+z0+(z1-z0)*t));}
      const visible=Math.cos((a+b)/2)>=0;
      if(job.mode==='laser')ctx.strokeStyle=`rgba(152,59,17,${(.18+.82*s[5]/peakPower)*(visible?1:.42)})`;
      else ctx.strokeStyle=visible?'#126874':'#82acae';
      ctx.lineWidth=visible?1.8:1;ctx.stroke();
    }
  }
  $('rotaryOrbit').oninput=drawCylinder;
  $('rotaryProgress').oninput=()=>{stop();drawCylinder();};
  $('rotaryPlay').onclick=()=>{
    if(animation){stop();return;}
    const slider=$('rotaryProgress');if(Number(slider.value)>=100)slider.value=0;
    const start=performance.now(), initial=Number(slider.value);$('rotaryPlay').textContent='Pause simulation';
    function frame(now){slider.value=Math.min(100,initial+(now-start)/60);drawCylinder();if(Number(slider.value)<100)animation=requestAnimationFrame(frame);else stop();}
    animation=requestAnimationFrame(frame);
  };
  function current(project,job) {
    if(!reviewed || reviewed.job!==job || reviewed.document!==JSON.stringify(project)|| JSON.stringify(options())!==JSON.stringify(reviewed.options))throw Error('Rotary setup or design changed. Generate a new wrapped preview.');
    return reviewed;
  }
  function request(project, job) {
    const {options:o}=current(project,job);
    if(!window.studioSession?.rotary_stream) throw Error('This workstation does not support rotary streaming. Update its service or export the file.');
    if(!['grblhal','xpro-v5'].includes(o.postprocessor)||project.protocol!==o.postprocessor)throw Error('Match the machine protocol to the grblHAL or xPRO V5 rotary postprocessor.');
    if(!latest.connected || latest.protocol!==o.postprocessor || !latest.rotary_axes?.includes(o.output_axis))throw Error('The connected controller must report the confirmed independent rotary axis.');
    if(!$('rotaryConfirmed').checked)throw Error('Confirm the independent rotary drive, degree calibration, work zero and fixture in Rotary.');
    return {rotary:o,rotary_confirmed:true};
  }
  window.KerfKiteRotary={clear,current,request};
  run('rotaryPreview',async()=>{
    invalidate();const jobRevision=revision,epoch=previewEpoch;
    const o=options(), project=machiningProject(),document=JSON.stringify(project);
    report('Building wrapped toolpath…');
    const job=await api('rotary',{project,options:o});
    if(jobRevision!==revision || epoch!==previewEpoch || document!==JSON.stringify(machiningProject()) || JSON.stringify(o)!==JSON.stringify(options()))throw Error('The setup changed while posting. Preview again.');
    if(!job.rotary || !Array.isArray(job.segments)||!job.segments.length)throw Error('The server did not return a wrapped toolpath.');
    reviewed={job,options:o,document,maxPower:job.segments.reduce((m,s)=>Math.max(m,s[5]||0),0)};
    preview=job;previewProject=document;
    const angles=job.rotary.angle_bounds.map(v=>v.toFixed(3)).join(' to '), count=job.lines.length;
    const description=`${o.postprocessor} · ${job.mode} · ${o.output_axis} ${angles}° · ${count} posted lines · ${job.segments.length} cutting segments`;
    $('rotaryMetrics').textContent=description;$('rotaryCode').textContent=job.lines.slice(0,120).join('\n')+(count>120?'\n…':'');
    $('summary').textContent=description+' · Wrapped program selected.';$('gcode').textContent=job.lines.join('\n');
    $('rotaryResult').hidden=false;$('rotaryProgress').value=100;renderToolpaths();drawCylinder();enabled();
    report((job.warnings||[]).join(' '));
  });
  run('rotaryExport',()=>{
    const {job,options:o}=current(machiningProject(),preview);
    download('rotary-'+o.postprocessor+(o.postprocessor==='linuxcnc'?'.ngc':'.nc'),job.lines.join('\n')+'\n'); report('Exported the reviewed wrapped program.');
  });
  run('rotaryRun',async()=>{
    if(busy)return;
    const project=machiningProject(), extra=request(project,preview);
    if(!$('rotaryConfirmed').checked)throw Error('Review the wrapped program and confirm the machine setup.');
    if(latest.state!=='Idle'||latest.active)throw Error('Wait for the connected controller to be idle.');
    busy=true;$('rotaryConfirmed').checked=false;enabled();
    try{await api('run',{project,...extra,confirmed:true});report('Submitted the reviewed rotary program. Monitor the controller status.');}
    finally{busy=false;enabled();}
  });
  load();
})();
