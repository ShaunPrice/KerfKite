/* Feature history and camera belong to the CAD workspace. Only the editable
 * model is saved with the project. Machining transfers create 2D snapshots. */
(() => {
  'use strict';
  const panel = $('design-panel-solid');
  const workspace = document.querySelector('main > .workspace');
  const stage = document.createElement('section');
  stage.className = 'solid-stage';
  stage.hidden = true;
  workspace.append(stage);
  const types = {box:'Box', cylinder:'Cylinder / cone', sphere:'Sphere', extrude:'Sketch extrusion', revolve:'Sketch revolution'};
  const operations = {add:'Join', cut:'Cut', intersect:'Intersect'};
  const empty = () => ({version:1,parameters:{},features:[]});
  const clone = value => JSON.parse(JSON.stringify(value));
  const view = {yaw:-.72, pitch:.6, zoom:1};
  let model = empty(), index = -1, mesh = null, revision = 0, timer, request = 0;
  let history = [], future = [], lesson = null, loadFailed = false;

  panel.innerHTML = `
    <h2>Part design</h2>
    <div class="solid-group">
      <h3>Parameters</h3><div id="solid-params"></div>
      <label>New parameter<input id="solid-param-name" maxlength="64" placeholder="e.g. plate_width"></label>
      <button id="solid-add-param" type="button">Add parameter</button>
    </div>
    <div class="solid-group">
      <h3>Features</h3><div id="solid-tree" class="solid-tree" aria-label="Feature history"></div>
      <label>Feature type<select id="solid-kind"></select></label>
      <button id="solid-add" type="button">Add feature</button>
    </div>
    <div id="solid-form" class="solid-group"></div>
    <div class="buttons"><button id="solid-undo">Undo CAD edit</button><button id="solid-redo">Redo</button></div>`;
  stage.innerHTML = `
    <div class="solid-top"><h1>Solid modeller</h1><button id="solid-guide">Guided plate tutorial</button></div>
    <section id="solid-lesson" hidden><strong id="solid-lesson-title"></strong><p id="solid-instruction"></p>
      <button id="solid-keep" hidden>Keep lesson model</button> <button id="solid-exit">Exit tutorial</button></section>
    <div class="solid-view"><canvas id="solid-canvas" tabindex="0" aria-label="3D model viewport. Arrow keys orbit. Plus and minus zoom."></canvas>
      <span class="solid-view-units">mm</span><span class="solid-view-hint">Drag to orbit · Scroll to zoom</span></div>
    <div class="solid-tools"><button id="solid-fit">Fit isometric</button><button id="solid-top">Top view</button>
      <button id="solid-build">Rebuild</button><button id="solid-export">Export STL</button></div>
    <p id="solid-status" role="status"></p>
    <dl class="solid-metrics"><div><dt>Overall size (mm)</dt><dd id="solid-size">—</dd></div><div><dt>Volume (mm³)</dt><dd id="solid-volume">—</dd></div></dl>
    <section><h3>Prepare machining</h3>
      <div class="solid-pair"><label>Placement X (mm)<input id="solid-x" type="number" min="0" value="10" step="any"></label>
      <label>Placement Y (mm)<input id="solid-y" type="number" min="0" value="10" step="any"></label></div>
      <label>Machining process<select id="solid-process"><option value="router">Router contour</option><option value="laser">Laser outline</option><option value="knife">Knife outline</option><option value="relief">Router top relief</option></select></label>
      <label id="solid-outline-choice">Contour source<select id="solid-source"><option value="project">Whole part silhouette</option><option value="section">Horizontal section at Z</option></select></label>
      <label id="solid-slice-label" hidden>Section Z (mm)<input id="solid-slice" type="number" step="any" value="1"></label>
      <button id="solid-transfer">Prepare in CAM</button>
      <p class="hint">Placement sets the lower left of the part in the work area. Silhouettes include through holes. Sections include all boundaries at the chosen Z. Review the paths and their cutting depth in CAM. Each transfer creates an independent machining setup.</p></section>`;
  const kindSelect = $('solid-kind');
  for (const [key,label] of Object.entries(types)) kindSelect.add(new Option(label,key));
  function status(text, error=false) {
    $('solid-status').textContent = text;
    $('solid-status').dataset.error = String(error);
  }
  function button(label, action) {
    const control = document.createElement('button');
    control.type = 'button';
    control.textContent = label;
    control.onclick = action;
    return control;
  }
  function field(parent, labelText, value, change) {
    const label = document.createElement('label');
    label.append(document.createTextNode(labelText));
    const input = document.createElement('input');
    input.value = value;
    input.maxLength = 160;
    input.oninput = () => change(input.value);
    label.append(input);
    parent.append(label);
    return input;
  }
  function select(parent, labelText, choices, current, change) {
    const label = document.createElement('label');
    label.append(document.createTextNode(labelText));
    const widget = document.createElement('select');
    widget.setAttribute('aria-label', labelText);
    for (const [key,title] of Object.entries(choices)) widget.add(new Option(title,key));
    widget.value = current;
    widget.onchange = () => change(widget.value);
    label.append(widget);
    parent.append(label);
  }
  function controls() {
    $('solid-undo').disabled = !history.length;
    $('solid-redo').disabled = !future.length;
    $('solid-transfer').disabled = !mesh || Boolean(lesson);
    $('solid-export').disabled = !mesh;
    $('solid-build').disabled = !model.features.length || loadFailed || offline;
  }
  // Group continuous typing into a single undo step, preserving independent edits.
  let lastField = null, lastEditTime = 0;
  function edit(change, rerender=true, source=null) {
    if (loadFailed) return;
    if (!source || source !== lastField || Date.now()-lastEditTime > 1000) {
      history.push(JSON.stringify(model));
      if (history.length > 40) history.shift();
    }
    future = [];
    lastField = source;
    lastEditTime = Date.now();
    change();
    store();
    if (rerender) forms();
    invalidateSolid();
  }
  function store() {
    if (!lesson) {
      window.projectExtras.solidCAD = clone(model);
      window.dispatchEvent(new Event('cnc-project-changed'));
    }
  }
  function invalidateSolid() {
    revision++;
    request++;
    clearTimeout(timer);
    mesh = null;
    $('solid-size').textContent = '—';
    $('solid-volume').textContent = '—';
    paint();
    controls();
    if (offline) {status('Open this project in the Studio app server to rebuild solids or prepare machining.'); return;}
    if (!model.features.length) {status('Choose a feature type and add the first solid.'); return;}
    status('Rebuild pending…');
    if (!stage.hidden) timer = setTimeout(rebuild, 350);
  }
  function restoreHistory(from,to) {
    if (!from.length) return;
    to.push(JSON.stringify(model));
    model = JSON.parse(from.pop());
    lastField=null;
    index=Math.min(index,model.features.length-1);
    store(); forms(); invalidateSolid();
  }
  $('solid-undo').onclick=()=>restoreHistory(history,future);
  $('solid-redo').onclick=()=>restoreHistory(future,history);
  $('solid-add-param').onclick=()=>{
    const name=$('solid-param-name').value.trim();
    if (!/^[A-Za-z][A-Za-z0-9_]{0,31}$/.test(name) || name in model.parameters) {
      status('Choose a unique parameter name starting with a letter, up to 32 characters.',true);
      return;
    }
    edit(()=>{model.parameters[name]=10;});
    $('solid-param-name').value='';
  };
  function params() {
    const host=$('solid-params'); host.replaceChildren();
    for (const [name,value] of Object.entries(model.parameters)) {
      const row=document.createElement('div'); row.className='solid-param';
      const input=field(row,name,value,next=>edit(()=>{model.parameters[name]=next;},false,input));
      const remove=button('×',()=>edit(()=>{delete model.parameters[name];}));
      remove.setAttribute('aria-label','Remove parameter '+name);
      row.append(remove);host.append(row);
    }
  }
  function makeFeature(kind) {
    let n=1;
    while(model.features.some(f=>f.id===kind+n)) n++;
    const common={id:kind+n,kind,op:'add',origin:[0,0,0],rotation:[0,0,0]};
    switch(kind) {
      case 'box': return {...common,size:[60,32,6]};
      case 'cylinder': return {...common,radius:4,height:8,origin:[12,12,-1]};
      case 'sphere': return {...common,radius:10};
      case 'extrude': return {...common,height:8,points:[[0,0],[30,0],[15,20]]};
      case 'revolve': return {...common,angle:360,points:[[4,0],[14,0],[10,18],[4,18]]};
    }
  }
  $('solid-add').onclick=()=>edit(()=>{
    model.features.push(makeFeature(kindSelect.value));
    index=model.features.length-1;
  });
  function features() {
    const tree=$('solid-tree');tree.replaceChildren();
    model.features.forEach((feature,i)=>{
      const row=button(feature.id,()=>{index=i;features();featureForm();});
      const tag=document.createElement('small');tag.textContent=feature.enabled===false?'Suppressed':operations[feature.op]||'Join';
      row.append(tag);row.setAttribute('aria-pressed',String(index===i));tree.append(row);
    });
  }
  function featureForm() {
    const host=$('solid-form');host.replaceChildren();
    const f=model.features[index];if (!f) return;
    const title=document.createElement('h3');title.textContent=f.kind==='step'?'Imported STEP solid':(types[f.kind]||'Unknown feature');host.append(title);
    const name=field(host,'Feature name',f.id,value=>{edit(()=>{f.id=value;},false,name);features();});
    select(host,'Boolean operation',operations,f.op||'add',op=>edit(()=>{f.op=op;}));
    function scalar(label,key,value) {
      const input=field(host,label,value,next=>edit(()=>{f[key]=next;},false,input));
    }
    function vector(container,key,labels,values) {
      const row=document.createElement('div');row.className=labels.length===3?'solid-triple':'solid-pair';container.append(row);
      labels.forEach((label,i)=>{
        const input=field(row,label,values[i],next=>edit(()=>{
          if (!f[key]) f[key]=[...values];
          f[key][i]=next;
        },false,input));
      });
    }
    if (f.kind==='box') vector(host,'size',['Length X','Width Y','Height Z'],f.size);
    if (['cylinder','sphere'].includes(f.kind)) scalar('Radius','radius',f.radius);
    if (['cylinder','extrude'].includes(f.kind)) scalar('Height','height',f.height);
    if (f.kind==='cylinder') {
      if ('topRadius' in f) {scalar('Top radius','topRadius',f.topRadius);host.append(button('Use equal radii',()=>edit(()=>delete f.topRadius)));}
      else host.append(button('Taper cylinder',()=>edit(()=>{f.topRadius=f.radius;})));
    }
    if (f.kind==='revolve') scalar('Sweep angle (°)','angle',f.angle??360);
    if (Array.isArray(f.points)) {
      host.append(button('Edit sketch on plane',async()=>{
        const original=revision;
        try {
          const points=await window.editSolidSketch({feature:clone(f),parameters:clone(model.parameters)});
          if(!points)return;
          if(revision!==original || model.features[index]!==f)throw Error('The feature changed while the sketch was open. Reopen the editor.');
          edit(()=>{f.points=points;});
        } catch(error){status(error.message,true);}
      }));
      const profile=document.createElement('details');profile.open=true;
      const summary=document.createElement('summary');summary.textContent=f.kind==='revolve'?'Profile in radius and Z':'Sketch points in XY';profile.append(summary);
      f.points.forEach((point,i)=>{
        const row=document.createElement('div');row.className='solid-point';
        point.forEach((value,axis)=>{
          const label=(f.kind==='revolve'?['R','Z']:['X','Y'])[axis]+' '+(i+1);
          const input=field(row,label,value,next=>edit(()=>{point[axis]=next;},false,input));
        });
        const drop=button('×',()=>edit(()=>{f.points.splice(i,1);}));
        drop.disabled=f.points.length<=3;drop.setAttribute('aria-label','Remove point '+(i+1));row.append(drop);profile.append(row);
      });
      profile.append(button('Add point',()=>edit(()=>{f.points.push([0,10]);})));host.append(profile);
    }
    const placement=document.createElement('details');
    const summary=document.createElement('summary');summary.textContent='Position and rotation';placement.append(summary);
    vector(placement,'origin',['X','Y','Z'],f.origin||[0,0,0]);vector(placement,'rotation',['X°','Y°','Z°'],f.rotation||[0,0,0]);host.append(placement);
    const patternBox=document.createElement('details');
    patternBox.open=Boolean(f.pattern);
    const patternTitle=document.createElement('summary');patternTitle.textContent='Pattern copies';patternBox.append(patternTitle);
    select(patternBox,'Pattern',{none:'Single feature',linear:'Linear pattern',polar:'Polar pattern'},f.pattern?.kind||'none',kind=>edit(()=>{
      if(kind==='none') delete f.pattern;
      else f.pattern=kind==='linear'?{kind,count:3,step:[10,0,0]}:{kind,count:4,angle:90,center:[0,0,0]};
    }));
    if(f.pattern) {
      const pattern=f.pattern;
      function property(label,key) {
        const input=field(patternBox,label,pattern[key],next=>edit(()=>{pattern[key]=next;},false,input));
      }
      property('Instance count','count');
      if(pattern.kind==='polar')property('Angle per copy (°)','angle');
      const row=document.createElement('div');row.className='solid-triple';patternBox.append(row);
      const key=pattern.kind==='linear'?'step':'center';
      ['X','Y','Z'].forEach((axis,i)=>{
        const values=pattern[key]||[0,0,0];
        const input=field(row,(key==='step'?'Step ':'Centre ')+axis,values[i],next=>edit(()=>{
          if(!pattern[key]) pattern[key]=values;
          pattern[key][i]=next;
        },false,input));
      });
    }
    host.append(patternBox);
    const actions=document.createElement('div');actions.className='solid-actions';
    function move(step) {edit(()=>{const dest=index+step;[model.features[index],model.features[dest]]=[model.features[dest],model.features[index]];index=dest;});}
    const up=button('Earlier',()=>move(-1));up.disabled=index===0;
    const down=button('Later',()=>move(1));down.disabled=index===model.features.length-1;
    actions.append(up,down,button(f.enabled===false?'Enable':'Suppress',()=>edit(()=>{f.enabled=f.enabled===false;})),
      button('Remove',()=>edit(()=>{model.features.splice(index,1);index=Math.min(index,model.features.length-1);})));
    host.append(actions);
  }
  function forms(){params();features();featureForm();controls();}
  async function rebuild() {
    clearTimeout(timer);
    if (offline || loadFailed || !model.features.length) return;
    const ticket=++request;
    status('Rebuilding solid…');
    try {
      const result=await api('solid-cad',{model});
      if (ticket!==request) return;
      mesh=result;
      const dims=[0,1,2].map(i=>mesh.bounds[i+3]-mesh.bounds[i]);
      $('solid-size').textContent=dims.map(val=>Number(val.toFixed(2))).join(' × ');
      $('solid-volume').textContent=mesh.volume.toLocaleString(undefined,{maximumFractionDigits:2});
      status('Solid ready.');
      updateLesson();
    } catch(error) {
      if (ticket!==request) return;
      mesh=null;status(error.message,true);
    }
    paint();controls();
  }
  $('solid-build').onclick=rebuild;
  const canvas=$('solid-canvas');
  function paint() {
    if(stage.hidden) return;
    const rect=canvas.getBoundingClientRect();
    if(!rect.width || !rect.height) return;
    const pixelRatio=Math.min(window.devicePixelRatio||1,2);
    const w=Math.round(rect.width*pixelRatio), h=Math.round(rect.height*pixelRatio);
    if(canvas.width!==w || canvas.height!==h){canvas.width=w;canvas.height=h;}
    const ctx=canvas.getContext('2d');
    if (!ctx) return;
    ctx.clearRect(0,0,w,h);
    if(!mesh){
      ctx.fillStyle='#d6e2e8';ctx.textAlign='center';ctx.font=(14*pixelRatio)+'px system-ui';
      ctx.fillText(model.features.length?'Awaiting solid geometry':'Add a feature to start modelling',w/2,h/2);
      return;
    }
    const centre=[0,1,2].map(i=>(mesh.bounds[i+3]+mesh.bounds[i])/2);
    const diagonal=Math.hypot(...[0,1,2].map(i=>mesh.bounds[i+3]-mesh.bounds[i]));
    const scale=Math.min(w,h)*.82*view.zoom/Math.max(.001,diagonal);
    const ca=Math.cos(view.yaw),sa=Math.sin(view.yaw),cb=Math.cos(view.pitch),sb=Math.sin(view.pitch);
    function rotate(x,y,z) {
      const horizontal=ca*x-sa*y, depth=sa*x+ca*y;
      return [horizontal,sb*depth+cb*z,-cb*depth+sb*z];
    }
    const pts=mesh.vertices.map(v=>rotate(v[0]-centre[0],v[1]-centre[1],v[2]-centre[2]));
    const faces=mesh.triangles.map(tri=>{
      const [a,b,c]=tri.map(i=>pts[i]);
      const ab=b.map((v,i)=>v-a[i]),ac=c.map((v,i)=>v-a[i]);
      const normal=[ab[1]*ac[2]-ab[2]*ac[1],ab[2]*ac[0]-ab[0]*ac[2],ab[0]*ac[1]-ab[1]*ac[0]];
      const length=Math.hypot(...normal)||1;
      const light=Math.max(0,(-.32*normal[0]+.56*normal[1]+.72*normal[2])/length);
      return {tri,depth:a[2]+b[2]+c[2],light};
    }).sort((a,b)=>a.depth-b.depth);
    for(const {tri,light} of faces){
      ctx.beginPath();
      tri.forEach((i,j)=>{const p=pts[i];ctx[j===0?'moveTo':'lineTo'](w/2+p[0]*scale,h/2-p[1]*scale);});
      ctx.closePath();ctx.fillStyle='hsl(188 40% '+(43+light*33)+'%)';
      ctx.fill();ctx.strokeStyle=ctx.fillStyle;ctx.lineWidth=.6*pixelRatio;ctx.stroke();
    }
    ctx.lineWidth=2*pixelRatio;ctx.textAlign='center';ctx.font=(10*pixelRatio)+'px system-ui';
    [[1,0,0,'X','#ffa997'],[0,1,0,'Y','#aad697'],[0,0,1,'Z','#8dcdf1']].forEach(([x,y,z,symbol,color])=>{
      const v=rotate(x,y,z), ox=35*pixelRatio,oy=35*pixelRatio;
      ctx.strokeStyle=ctx.fillStyle=color;ctx.beginPath();ctx.moveTo(ox,oy);ctx.lineTo(ox+18*pixelRatio*v[0],oy-18*pixelRatio*v[1]);ctx.stroke();
      ctx.fillText(symbol,ox+26*pixelRatio*v[0],oy-26*pixelRatio*v[1]+3*pixelRatio);
    });
  }
  new ResizeObserver(paint).observe(canvas);
  let drag=null;
  canvas.onpointerdown=e=>{drag=[e.clientX,e.clientY];canvas.setPointerCapture(e.pointerId);};
  function orbit(dx,dy) {
    view.yaw+=dx;view.pitch=Math.max(-Math.PI/2,Math.min(Math.PI/2,view.pitch+dy));paint();
  }
  canvas.onpointermove=e=>{if(drag){orbit((e.clientX-drag[0])*.012,(e.clientY-drag[1])*.012);drag=[e.clientX,e.clientY];}};
  canvas.onlostpointercapture=()=>drag=null;
  function zoom(factor){view.zoom=Math.max(.25,Math.min(6,view.zoom*factor));paint();}
  canvas.addEventListener('wheel',e=>{e.preventDefault();zoom(Math.exp(-e.deltaY*.0015));},{passive:false});
  canvas.onkeydown=e=>{
    const key=e.key;
    if(!['ArrowLeft','ArrowRight','ArrowUp','ArrowDown','+','=','-'].includes(key))return;
    e.preventDefault();e.stopPropagation();
    if(key.startsWith('Arrow'))orbit((key==='ArrowLeft'?-.1:key==='ArrowRight'?.1:0),(key==='ArrowDown'?-.1:key==='ArrowUp'?.1:0));
    else zoom(key==='-'?.9:1.1);
  };
  $('solid-fit').onclick=()=>{Object.assign(view,{yaw:-.72,pitch:.6,zoom:1});paint();};
  $('solid-top').onclick=()=>{Object.assign(view,{yaw:0,pitch:Math.PI/2,zoom:1});paint();};
  async function stl() {
    if (!mesh) throw Error('Rebuild the solid before exporting.');
    const stamp=revision;
    const response=await fetch('/api/solid-cad/stl',{method:'POST',headers:{'Content-Type':'application/json','X-Studio-Token':token},body:JSON.stringify({model})});
    if(!response.ok) {
      const failure=await response.json().catch(()=>({}));
      throw Error(failure.error||'STL export failed.');
    }
    const file=await response.blob();
    if(stamp!==revision || !mesh)throw Error('The model changed. Rebuild and try again.');
    return file;
  }
  function attempt(action){return async()=>{try{await action();}catch(e){status(e.message,true);}};}
  $('solid-export').onclick=attempt(async()=>download('solid-part.stl',await stl(),'model/stl'));
  function updateSource() {
    const contours=$('solid-process').value!=='relief';
    $('solid-outline-choice').hidden=!contours;
    $('solid-slice-label').hidden=!contours || $('solid-source').value!=='section';
  }
  $('solid-process').onchange=updateSource;
  $('solid-source').onchange=updateSource;
  $('solid-transfer').onclick=attempt(async()=>{
    if(!mesh || lesson) return;
    const [x,y]=['solid-x','solid-y'].map(id=>$(id).valueAsNumber);
    const [x0,y0,z0,x1,y1,z1]=mesh.bounds;
    if(![x,y].every(Number.isFinite)||x<0||y<0||x+x1-x0>Number($('width').value)+1e-8||y+y1-y0>Number($('height').value)+1e-8)
      throw Error('Set a placement that fits the part inside the work area.');
    const process=$('solid-process').value;
    if(process==='relief') {
      const file=await stl();
      const transfer=new DataTransfer();
      transfer.items.add(new File([file],'solid-part.stl',{type:'model/stl'}));
      $('reliefFile').files=transfer.files;
      for(const [id,val] of Object.entries({shapeX:x,shapeY:y,shapeW:x1-x0,shapeH:y1-y0,reliefDepth:z1-z0}))$(id).value=val;
      $('mode').value='router';modeUI();invalidate();
      CNCToolTabs.open('design','shapes');CNCToolTabs.open('machine','relief');
      message('Part loaded at its model dimensions. Review cutter, stepover and depth, then generate the relief.');
      return;
    }
    let rings=mesh.profiles, name='XY silhouette';
    if ($('solid-source').value==='section') {
      const stamp=revision, z=$('solid-slice').valueAsNumber;
      if(!Number.isFinite(z))throw Error('Enter a finite section Z coordinate.');
      const result=await api('solid-cad/section',{model,z});
      if(stamp!==revision || !mesh)throw Error('The model changed. Rebuild and try again.');
      rings=result.profiles;name='Section Z '+z+' mm';
    }
    const paths=rings.map(path=>path.map(([px,py])=>[x+(px-x0),y+(py-y0)]));
    if(!paths.length)throw Error('No contours at the selected plane.');
    addGenerated('Solid · '+name,paths);
    $('mode').value=process;$('operation').value='line';modeUI();invalidate();
    CNCToolTabs.open('design','properties');CNCToolTabs.open('machine','cam');
    message(name+' added at model scale. Review tool settings and generate a preview.');
  });
  function updateLesson() {
    if(!lesson)return;
    const width=mesh?.parameters?.width;
    const bore=model.features.find(f=>f.id==='bore');
    if(lesson.step===0 && width===80)lesson.step=1;
    if(lesson.step===1 && width===80 && bore?.enabled===false && mesh)lesson.step=2;
    if(lesson.step===2 && width===80 && bore?.enabled!==false && mesh)lesson.step=3;
    const texts=[
      'Set the width parameter to 80 and wait for the solid to rebuild. Check the overall X dimension.',
      'Select bore in the feature list and choose Suppress. Watch the through hole disappear.',
      'Choose Enable on the bore to restore the cut. Compare the volume.',
      'The rebuilt plate measures 80 mm across and has a through hole. Keep it to prepare a machining operation, or exit to return to your part.'
    ];
    $('solid-lesson-title').textContent=lesson.step===3?'Plate tutorial complete':'Plate tutorial · '+(lesson.step+1)+' of 3';
    $('solid-instruction').textContent=texts[lesson.step];
    $('solid-keep').hidden=lesson.step!==3;
  }
  function endLesson(keep) {
    const previous=lesson;lesson=null;
    if(!keep){
      model=previous.model;history=previous.history;future=previous.future;index=previous.index;
    } else {
      history=[...previous.history,JSON.stringify(previous.model)].slice(-40);future=[];store();
    }
    lastField=null;$('solid-lesson').hidden=true;$('solid-guide').disabled=false;
    forms();invalidateSolid();
  }
  $('solid-guide').onclick=()=>{
    if(lesson || loadFailed)return;
    lesson={model,history,future,index,step:0};
    model={version:1,parameters:{width:60},features:[
      {id:'plate',kind:'box',op:'add',size:['width',32,6]},
      {id:'bore',kind:'cylinder',op:'cut',radius:4,height:8,origin:['width / 2',16,-1]}
    ]};
    index=0;history=[];future=[];lastField=null;
    $('solid-lesson').hidden=false;$('solid-guide').disabled=true;updateLesson();forms();invalidateSolid();
  };
  $('solid-keep').onclick=()=>endLesson(true);
  $('solid-exit').onclick=()=>endLesson(false);
  function syncTab() {
    const active=!panel.hidden;
    workspace.classList.toggle('solid-active',active);stage.hidden=!active;
    if(active && !loadFailed){if(!mesh)invalidateSolid();else paint();}
  }
  function load() {
    clearTimeout(timer);revision++;request++;mesh=null;loadFailed=false;
    history=[];future=[];lesson=null;lastField=null;index=-1;
    $('solid-lesson').hidden=true;$('solid-guide').disabled=false;
    model=empty();
    try {
      const saved=window.projectExtras?.solidCAD;
      if(saved!=null) {
        if(saved.version!==1 || !Array.isArray(saved.features) || typeof saved.parameters!=='object' || !saved.parameters)throw Error('Unsupported CAD project format.');
        model=clone(saved);
      }
      index=model.features.length?0:-1;
      forms();invalidateSolid();
    }catch(error){
      model=empty();forms();loadFailed=true;controls();status(error.message,true);paint();
    }
  }
  window.CNCSolidExchange = {
    model: () => clone(model),
    async addStep(content) {
      if (lesson || loadFailed) throw Error('Exit the part lesson or open a supported project before adding STEP geometry.');
      if (model.features.length >= 64) throw Error('A part may contain up to 64 features.');
      let n=1;
      while (model.features.some(f=>f.id==='step'+n)) n++;
      const feature={id:'step'+n, kind:'step', op:'add', content};
      const proposed=clone(model);
      proposed.features.push(feature);
      const start=revision;
      await api('solid-cad',{model:proposed});
      if (revision!==start) throw Error('The part changed during import. Review it before adding the STEP solid.');
      edit(()=>{
        model.features.push(feature);
        index=model.features.length-1;
      });
    }
  };
  window.addEventListener('cnc-tab-changed',syncTab);
  window.addEventListener('cnc-project-loaded',load);
  load();syncTab();
})();
