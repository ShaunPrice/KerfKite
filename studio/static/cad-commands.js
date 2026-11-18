/* Keyboard authoring and editable script geometry. All operations act on design data. */
(() => {
  'use strict';
  const consoleBar=document.createElement('section');
  consoleBar.className='cad-console';
  consoleBar.innerHTML=`<form id="cad-command-form"><label for="cad-command">Command <kbd>/</kbd></label>
    <input id="cad-command" autocomplete="off" spellcheck="false" placeholder="rect 60 30 or view top">
    <button type="submit">Run</button><button type="button" id="cad-script-open">Script…</button>
    <button type="button" id="cad-command-help" aria-label="Command reference" aria-expanded="false">?</button></form>
    <output id="cad-command-result" aria-live="polite"></output>
    <div id="cad-reference" hidden><b>Draw in mm</b> <code>rect width height [x y]</code> · <code>circle radius [cx cy]</code> · <code>line x1 y1 x2 y2</code>
    <br><b>Navigate</b> <code>view front</code> · <code>top</code> · <code>iso</code> · <code>fit drawing</code> · <code>pan dx dy</code> · <code>zoom 1.5</code> · <code>orbit 15 10</code>
    <br><b>Model</b> <code>cube([40,25,5]);</code> · <code>script</code> · <code>edit</code> · <code>macros</code> · <code>macro name width=50</code>. Use ↑ and ↓ in the command line for history.</div>`;
  document.querySelector('main > .workspace').prepend(consoleBar);
  const dialog=document.createElement('dialog');
  dialog.id='cad-script-dialog'; dialog.setAttribute('aria-labelledby','cad-script-title');
  dialog.innerHTML=`<div class="cad-script-head"><div><h2 id="cad-script-title">CAD script</h2><p id="cad-script-target"></p></div>
    <button id="cad-script-close" aria-label="Close CAD script editor">Close</button></div>
    <div class="cad-script-tools"><button id="cad-script-new">New</button><button id="cad-script-load">Open .scad…</button><input type="file" accept=".scad" id="cad-script-file" hidden>
    <button id="cad-script-save">Save .scad</button><select id="cad-script-examples" aria-label="Load a script example">
    <option value="">Examples…</option><option value="drawing">Slotted tag · 2D</option><option value="plate">Mounting plate · 3D</option><option value="knob">Fluted knob · 3D</option></select></div>
    <div class="cad-script-panes"><div class="cad-code-pane"><label for="cad-script-source">Source</label>
    <textarea id="cad-script-source" spellcheck="false" autocomplete="off" autocapitalize="off" aria-describedby="cad-script-feedback"></textarea></div>
    <div class="cad-preview-pane"><div class="cad-preview-title"><strong>Geometry preview</strong><button id="cad-preview-fit">Reset view</button></div>
    <div class="mesh-viewport cad-script-stage"><canvas id="cad-script-canvas" aria-label="Script geometry preview, drag a solid to orbit"></canvas></div>
    <output id="cad-script-metrics"></output></div></div>
    <div class="cad-script-bottom"><output id="cad-script-feedback" role="status"></output>
    <button id="cad-script-preview">Preview <kbd>F5</kbd></button><button class="primary" id="cad-script-apply">Apply <kbd>Ctrl ↵</kbd></button></div>`;
  document.body.append(dialog);
  const source=$('cad-script-source'), canvas=$('cad-script-canvas'), command=$('cad-command');
  const examples={
    drawing:'width = 60;\nheight = 24;\n\ndifference() {\n  square([width, height]);\n  translate([8, height/2]) circle(d=5);\n}\n',
    plate:'width = 60;\nlength = 32;\nthickness = 5;\n\nmodule bolt_hole(x, y) {\n  translate([x, y, -1])\n    cylinder(h=thickness + 2, d=4, $fn=48);\n}\n\ndifference() {\n  cube([width, length, thickness]);\n  for (x = [6, width-6])\n    for (y = [6, length-6]) bolt_hole(x, y);\n}\n',
    knob:'$fn = 64;\nflutes = 12;\n\ndifference() {\n  cylinder(h=14, r=16);\n  translate([0,0,-1]) cylinder(h=16, d=6);\n  for (i = [0:flutes-1])\n    rotate([0,0,i*360/flutes])\n      translate([16,0,-1]) cylinder(h=16, r=2);\n}\n'
  };
  let epoch=0, active=null, lastSource='', fileName='design.scad', preview=null, previewText='', request=0, applying=false;
  let yaw, pitch, magnification; const output=$('cad-script-feedback');
  const resetCamera=()=>{yaw=-.72;pitch=.6;magnification=1;};
  function tell(element,text,error=false){element.textContent=text;element.dataset.error=String(error);}
  function context(editSelected) {
    const model=CNCSolidExchange.model();
    const solidMode=KerfKiteViews.current()==='3d';
    const feature=editSelected && solidMode ? CNCSolidExchange.selectedScript() : null;
    const obj=editSelected && !solidMode && typeof objects[selected]?.scad?.source==='string' ? objects[selected] : null;
    return {epoch,feature,obj,original:obj?JSON.stringify(obj):null,model:JSON.stringify(model),
      overrides:obj?.scad.overrides||feature?.scriptParameters,parameters:obj?.scad.parameters||model.parameters,source:feature?.source??obj?.scad.source??''};
  }
  function isSameTarget(a,b){return a && a.epoch===b.epoch && a.feature?.id===b.feature?.id && a.obj===b.obj && a.model===b.model && a.original===b.original;}
  function validateContext(target) {
    if(target.epoch!==epoch)throw Error('The project changed. Reopen the editor to apply this script.');
    if(target.obj && (!objects.includes(target.obj)||JSON.stringify(target.obj)!==target.original))
      throw Error('The drawing object changed. Reopen its script before applying.');
    if(!target.obj && target.model!==JSON.stringify(CNCSolidExchange.model()))throw Error('The part changed. Reopen its script before applying.');
  }
  function clearPreview() {
    request++;preview=null;previewText='';
    KerfKiteMeshRenderer.clear(canvas);
    const ctx=canvas.getContext('2d');ctx.setTransform(1,0,0,1,0,0);ctx.clearRect(0,0,canvas.width,canvas.height);
    $('cad-script-metrics').textContent='';
  }
  function setSource(target,text,name='design.scad') {
    active=target;fileName=name;source.value=text;lastSource=text;clearPreview();resetCamera();
    $('cad-script-target').textContent=target.feature?'Editing feature · '+target.feature.id:target.obj?'Editing drawing · '+target.obj.name:'New geometry';
    tell(output,'Preview the source, then apply it to the project.');
  }
  function canReplaceDraft(){return !active || source.value===lastSource || confirm('Discard the unapplied script edits?');}
  function show(){if(!dialog.open)dialog.showModal();source.focus();}
  function openSelected(requireSelection=false) {
    if(applying)return;
    const target=context(true);
    if(requireSelection===true && !target.obj && !target.feature)throw Error("Select a scripted drawing or solid feature to edit.");
    if(!isSameTarget(active,target)) {
      if(!canReplaceDraft())return;
      setSource(target,target.source||examples.plate);
    }
    show();
  }
  async function openFile(file){
    if(!file || applying)return;
    if(file.size>65536)throw Error('Open a .scad file up to 64 KiB.');
    const start=epoch, text=await file.text();
    if(start!==epoch||applying)return;
    if(!canReplaceDraft())return;
    setSource(context(false),text,file.name);show();
  }
  function paintPreview(){
    if(!preview||!dialog.open)return;
    if(preview.dimension===3) {
      KerfKiteViews.paintMesh(canvas,preview,{yaw,pitch,zoom:magnification,panX:0,panY:0});
      return;
    }
    KerfKiteMeshRenderer.clear(canvas);
    const rect=canvas.getBoundingClientRect(),ratio=Math.min(devicePixelRatio||1,2);
    canvas.width=Math.max(1,Math.round(rect.width*ratio));canvas.height=Math.max(1,Math.round(rect.height*ratio));
    const ctx=canvas.getContext('2d'), [l,b,r,t]=preview.bounds;
    const scale=.83*magnification*Math.min(canvas.width/Math.max(r-l,.001),canvas.height/Math.max(t-b,.001));
    ctx.translate(canvas.width/2,canvas.height/2);ctx.scale(scale,-scale);ctx.translate(-(l+r)/2,-(b+t)/2);
    ctx.beginPath();for(const ring of preview.profiles){ring.forEach(([x,y],i)=>i?ctx.lineTo(x,y):ctx.moveTo(x,y));ctx.closePath();}
    ctx.fillStyle='#85cbc6';ctx.fill('evenodd');ctx.lineWidth=1.4*ratio/scale;ctx.strokeStyle='#b9eae6';ctx.stroke();
  }
  function sizes(result) {
    const n=result.dimension,b=result.bounds;
    return b.slice(0,n).map((x,i)=>Number((b[i+n]-x).toFixed(3))).join(' × ')+' mm';
  }
  async function compile(text,target){
    if(offline)throw Error('Open the installed Studio or hosted workshop to compile CAD scripts. The local file editor can open and save the source.');
    if(!text.trim())throw Error('Enter a CAD expression, for example cube([40,25,5]);');
    return api('cad-script',{source:text,parameters:target.parameters,overrides:target.overrides});
  }
  async function buildPreview(){
    validateContext(active);
    const text=source.value, ticket=++request;
    tell(output,'Building preview…');
    let result;
    try{result=await compile(text,active);}
    catch(error){if(ticket!==request||!dialog.open)return null;throw error;}
    if(ticket!==request||source.value!==text||!dialog.open)return null;
    validateContext(active);preview=result;previewText=text;paintPreview();
    $('cad-script-metrics').textContent=result.dimension+'D · '+sizes(result)+' · '+(result.dimension===2?result.area:result.volume).toLocaleString(undefined,{maximumFractionDigits:2})+(result.dimension===2?' mm²':' mm³');
    tell(output,result.dimension===3?'Drag to orbit. Scroll to zoom.':'Drawing preview · XY plane');
    return result;
  }
  // Recover the placement of an edited drawing from corresponding vertices before rebuilding it.
  function drawingMap(old,placed){
    if(old.length!==placed.length || old.some((r,i)=>r.length!==placed[i].length))
      throw Error('Drawing contours were edited. Use New to create a separate scripted drawing.');
    const from=old.flat(),to=placed.flat(),p=from[0],q=to[0];
    let j=0,k=0;
    const dist=([x,y])=>(x-p[0])**2+(y-p[1])**2;
    for(let i=1;i<from.length;i++)if(dist(from[i])>dist(from[j]))j=i;
    const u=from[j][0]-p[0],v=from[j][1]-p[1];
    const cross=([x,y])=>u*(y-p[1])-v*(x-p[0]);
    for(let i=1;i<from.length;i++)if(Math.abs(cross(from[i]))>Math.abs(cross(from[k])))k=i;
    const det=cross(from[k]);
    if(!Number.isFinite(det)||Math.abs(det)<1e-12)throw Error('The drawing is too narrow to restore its placement.');
    const map=([x,y])=>{
      const a=((x-p[0])*(from[k][1]-p[1])-(y-p[1])*(from[k][0]-p[0]))/det,b=cross([x,y])/det;
      return q.map((value,i)=>value+a*(to[j][i]-value)+b*(to[k][i]-value));
    };
    if(from.some((point,i)=>{const [x,y]=map(point);return Math.hypot(x-to[i][0],y-to[i][1])>1e-5;}))
      throw Error('Individual nodes have changed. Use New to keep the existing artwork.');
    return map;
  }
  async function commit(text,result,target,name){
    validateContext(target);
    if(result.dimension===3){
      if(target.obj)throw Error('This drawing produces a solid now. Use New to add it as a solid feature.');
      const id=await CNCSolidExchange.applyScript(text,target.feature?.id??null,target.model,{name:target.macroName,overrides:target.overrides});
      $('design-tab-solid').click();return 'Applied '+id+' · '+sizes(result);
    }
    if(target.feature)throw Error('This solid feature now produces a drawing. Use New to add the drawing separately.');
    let paths=result.profiles, placement='';
    if(target.obj){
      if(target.obj.powers||target.obj.depths)throw Error('This object contains per-vertex machining data. Use New to keep its machining data intact.');
      const old=await compile(target.source,target);
      validateContext(target);
      const map=drawingMap(old.profiles,target.obj.paths);
      paths=paths.map(r=>r.map(map));
    } else {
      const [l,b,r,t]=result.bounds,w=Number($('width').value),h=Number($('height').value);
      const dx=Math.max(-l,Math.min(0,w-r)),dy=Math.max(-b,Math.min(0,h-t));
      if(dx||dy){paths=paths.map(ring=>ring.map(([x,y])=>[x+dx,y+dy]));placement=' · placed on bed at offset '+[dx,dy].map(n=>Number(n.toFixed(3))).join(', ')+' mm';}
    }
    const [l,b,r,t]=bounds(paths);
    if(l<-.000001||b<-.000001||r>Number($('width').value)+.000001||t>Number($('height').value)+.000001)
      throw Error('The drawing exceeds the bed. Change the dimensions or placement before applying.');
    const other=objects.filter(o=>o!==target.obj).flatMap(o=>o.paths), total=[...other,...paths];
    if(total.length>3000||total.reduce((n,p)=>n+p.length,0)>100000)throw Error('This drawing exceeds the project path or vertex limit.');
    const scad={source:text,parameters:structuredClone(target.parameters),...(target.overrides?{overrides:structuredClone(target.overrides)}:{})};
    if(target.obj){
      selected=objects.indexOf(target.obj);objects[selected]={...target.obj,paths,scad};invalidate();draw();
    } else add(name||'Scripted drawing',paths,{scad});
    $('design-tab-shapes').click();return 'Applied drawing · '+sizes(result)+placement;
  }
  function fail(error){tell(output,error.message,true);}
  async function applyEditor(){
    if(applying)return; applying=true; source.readOnly=true;
    for(const b of dialog.querySelectorAll('button,select'))b.disabled=true;
    try {
      const text=source.value;
      const result=preview && previewText===text?preview:await buildPreview();
      if(!result)return;
      const message=await commit(text,result,active,fileName==='design.scad'?'Scripted drawing':fileName.replace(/\.scad$/i,''));
      active=null;dialog.close();tell($('cad-command-result'),message);
    } catch(e){fail(e);}
    finally{applying=false;source.readOnly=false;for(const b of dialog.querySelectorAll('button,select'))b.disabled=false;}
  }
  source.oninput=()=>{clearPreview();tell(output,'Source changed. Preview or apply to rebuild.');};
  $('cad-script-new').onclick=()=>{if(canReplaceDraft()){setSource(context(false),'');source.focus();}};
  $('cad-script-examples').onchange=e=>{
    const key=e.target.value;e.target.value='';
    if(examples[key]&&canReplaceDraft()){setSource(context(false),examples[key],key+'.scad');source.focus();}
  };
  $('cad-script-load').onclick=()=>$('cad-script-file').click();
  $('cad-script-file').onchange=async e=>{const file=e.target.files[0];e.target.value='';try{await openFile(file);}catch(err){fail(err);}};
  $('cad-script-save').onclick=()=>{download(fileName.toLowerCase().endsWith('.scad')?fileName:'design.scad',source.value,'text/plain');};
  $('cad-script-open').onclick=openSelected;
  $('cad-script-close').onclick=()=>dialog.close();
  dialog.addEventListener('cancel',e=>{if(applying)e.preventDefault();});
  dialog.addEventListener('close',()=>{request++;});
  $('cad-script-preview').onclick=()=>buildPreview().catch(fail);
  $('cad-script-apply').onclick=applyEditor;
  $('cad-preview-fit').onclick=()=>{resetCamera();paintPreview();};
  dialog.addEventListener('keydown',e=>{
    if(e.isComposing)return;
    if(e.key==='F5'||e.key==='F6'||((e.ctrlKey||e.metaKey)&&e.key==='Enter')){
      e.preventDefault();e.stopPropagation();if(applying)return;
      if(e.key==='F5')buildPreview().catch(fail);else applyEditor();
    }
  });
  let drag=null;
  canvas.onpointerdown=e=>{if(preview?.dimension===3&&e.button===0){canvas.setPointerCapture(e.pointerId);drag=[e.clientX,e.clientY];}};
  canvas.onpointermove=e=>{if(drag){yaw+=(e.clientX-drag[0])*.012;pitch=Math.max(-Math.PI/2,Math.min(Math.PI/2,pitch+(e.clientY-drag[1])*.012));drag=[e.clientX,e.clientY];paintPreview();}};
  canvas.onlostpointercapture=()=>{drag=null;};
  canvas.addEventListener('wheel',e=>{if(preview){e.preventDefault();magnification=Math.max(.2,Math.min(8,magnification*(e.deltaY<0?1.15:1/1.15)));paintPreview();}},{passive:false});
  canvas.addEventListener('mesh-renderer-changed',paintPreview);
  new ResizeObserver(paintPreview).observe(canvas);
  const history=[];let cursor=0, draft='',running=false;
  function reference(show){$('cad-reference').hidden=!show;$('cad-command-help').setAttribute('aria-expanded',String(show));}
  $('cad-command-help').onclick=()=>reference($('cad-reference').hidden);
  function numeric(tokens){
    if(tokens.some(t=>!/^[-+]?(?:\d+\.?\d*|\.\d+)(?:e[-+]?\d+)?$/i.test(t)||!Number.isFinite(Number(t))))throw Error('Enter finite numeric coordinates in millimetres.');
    return tokens.map(Number);
  }
  function drawingInstruction(action,tokens) {
    const n=numeric(tokens);
    let name,paths;
    if(action==='line'){
      if(n.length!==4||n[0]===n[2]&&n[1]===n[3])throw Error('Use line x1 y1 x2 y2 with distinct endpoints.');
      name='Line';paths=[[n.slice(0,2),n.slice(2)]];
    } else if(action==='circle'){
      if(![1,3].includes(n.length)||n[0]<=0)throw Error('Use circle radius [centreX centreY] with a positive radius.');
      const [r,x=r,y=r]=n;name='Circle';paths=[Array.from({length:129},(_,i)=>[x+r*Math.cos(i*Math.PI/64),y+r*Math.sin(i*Math.PI/64)])];
    } else {
      if(![2,4].includes(n.length)||n[0]<=0||n[1]<=0)throw Error('Use rect width height [x y] with positive dimensions.');
      const [w,h,x=0,y=0]=n;name='Rectangle';paths=[[[x,y],[x+w,y],[x+w,y+h],[x,y+h],[x,y]]];
    }
    const [left,bottom,right,top]=bounds(paths);
    const tolerance=1e-9;
    if(left < -tolerance || bottom < -tolerance || right > Number($('width').value)+tolerance || top > Number($('height').value)+tolerance)
      throw Error('Shape exceeds the work area. Change its dimensions or position.');
    return {name,paths};
  }
  const shapes=['rect','rectangle','circle','line'];
  function viewInstruction(action,args){
    const words=action==='view'?args:[action,...args];
    let name=(words[0]||'').toLowerCase(),values=words.slice(1);
    if(name==='isometric')name='iso';
    if(name==='fit'&&['bed','drawing'].includes(values[0]?.toLowerCase())){name='fit-'+values[0].toLowerCase();values=values.slice(1);}
    if(!['iso','top','bottom','front','back','right','left','fit','fit-bed','fit-drawing','zoom','orbit','pan'].includes(name))
      throw Error('Unknown command. Type help to see shapes and view commands.');
    const arity={pan:2,orbit:2,zoom:1}[name]||0;
    if(values.length!==arity)throw Error(name+' expects '+arity+' numeric arguments.');
    const numbers=numeric(values);
    if(numbers.some(n=>Math.abs(n)>1e6))throw Error('View arguments must be within ±1000000.');
    if(name==='zoom' && numbers[0]<=0)throw Error('Zoom factor must be positive.');
    return {name,args:numbers};
  }
  function macroCommands(text, parameters){
    const lines=text.split(/\r?\n/), plan=[];
    for(const [index,raw] of lines.entries()){
      const line=raw.replace(/(?:#|\/\/).*$/,'').trim();
      if(!line)continue;
      try{
        if(plan.length>=256)throw Error('Use at most 256 instructions.');
        const [first,...tokens]=line.split(/\s+/), action=first.toLowerCase();
        const args=tokens.map(token=>{
          const match=/^(?:\$([A-Za-z_]\w*)|\{([A-Za-z_]\w*)\})$/.exec(token);
          if(!match)return token;
          const key=match[1]||match[2];
          if(!Object.hasOwn(parameters,key))throw Error('Unknown parameter '+key+'.');
          return String(parameters[key]);
        });
        const instruction=shapes.includes(action)?{shape:drawingInstruction(action,args)}:{view:viewInstruction(action,args)};
        plan.push({...instruction,line:index+1});
      }catch(error){throw Error('Line '+(index+1)+': '+error.message);}
    }
    if(!plan.length)throw Error('Enter at least one drawing or view command.');
    return plan;
  }
  function applyCommandMacro(name,plan){
    const geometry=plan.flatMap(i=>i.shape?[i.shape]:[]);
    const paths=[...objects,...geometry].flatMap(o=>o.paths);
    if(paths.length>3000||paths.reduce((n,p)=>n+p.length,0)>100000)throw Error('Macro exceeds the project path or vertex limit.');
    const count=objects.length, previous=selected, layer=window.CNCDesignLayers?.activeId?.()||'default';
    const tab=document.querySelector('.design-tabs [aria-selected=true], [id^="design-tab-"][aria-selected=true]')?.id;
    const mode=KerfKiteViews.current(), drawing=KerfKiteViews.state('2d'), solid=KerfKiteViews.state('3d');
    let line=0;
    try{
      for(const instruction of plan){
        line=instruction.line;
        if(instruction.shape){
          objects.push({...instruction.shape,designLayerId:layer});
          selected=objects.length-1;
          if(KerfKiteViews.current()!=='2d')$('design-tab-shapes').click();
        }else KerfKiteViews.apply(instruction.view.name,instruction.view.args);
      }
    }catch(error){
      objects.splice(count);selected=previous;
      $(tab || (mode==='3d'?'design-tab-solid':'design-tab-shapes')).click();
      KerfKiteViews.restore(drawing,solid);draw();
      throw Error('Line '+line+': '+error.message);
    }
    if(geometry.length){invalidate();draw();}
    return name+' · '+geometry.length+' drawing object'+(geometry.length===1?'':'s')+' · '+plan.length+' command'+(plan.length===1?'':'s');
  }
  async function runMacro({name,kind,source:text,parameters={}}){
    if(typeof name!=='string'||!name.trim()||typeof text!=='string'||new TextEncoder().encode(text).length>65536)
      throw Error('Supply a named macro with up to 64 KiB of source.');
    if(!parameters||typeof parameters!=='object'||Array.isArray(parameters)||Object.keys(parameters).length>64||
       Object.entries(parameters).some(([key,value])=>!/^([A-Za-z_]\w{0,39})$/.test(key)||!Number.isFinite(value)||Math.abs(value)>100000))
      throw Error('Supply numeric macro parameters within ±100000.');
    if(kind==='commands')return applyCommandMacro(name,macroCommands(text,parameters));
    if(kind!=='scad')throw Error('Choose a drawing command sequence or a CAD script.');
    const target=context(false);
    target.overrides={...parameters};target.macroName=name;
    const result=await compile(text,target);
    return commit(text,result,target,name);
  }
  async function execute(line){
    const [first,...args]=line.trim().split(/\s+/),action=first.toLowerCase();
    if(action==='macros'){window.KerfKiteMacros.open();return 'Macro library opened.';}
    if(action==='macro'){
      if(!args.length){window.KerfKiteMacros.open();return 'Macro library opened.';}
      const parameters=Object.create(null);
      while(args.length && args.at(-1).includes('=')){
        const match=/^([A-Za-z_]\w*)=(.+)$/.exec(args.pop());
        if(!match || Object.hasOwn(parameters,match[1]))throw Error('Use a single name=value per macro parameter.');
        parameters[match[1]]=numeric([match[2]])[0];
      }
      return window.KerfKiteMacros.run(args.join(' '),parameters);
    }
    if(line.includes('(')){
      const target=context(false),result=await compile(line,target);
      return commit(line,result,target,'Scripted drawing');
    }
    if(shapes.includes(action)){
      const shape=drawingInstruction(action,args);
      add(shape.name,shape.paths);$('design-tab-shapes').click();
      return 'Added '+shape.name.toLowerCase()+'.';
    }
    if(action==='help'){reference(true);return 'Command reference opened.';}
    if(action==='script'||action==='edit'){openSelected(action==='edit');return 'CAD script editor opened.';}
    const view=viewInstruction(action,args);
    KerfKiteViews.apply(view.name,view.args);
    return 'View · '+(action==='view'?args.join(' '):line);
  }
  $('cad-command-form').onsubmit=async e=>{
    e.preventDefault();const line=command.value.trim();if(!line||running)return;
    if(history.at(-1)!==line)history.push(line);if(history.length>40)history.shift();cursor=history.length;draft='';
    running=true;command.readOnly=true;
    try {const message=await execute(line);command.value='';tell($('cad-command-result'),message);}
    catch(error){tell($('cad-command-result'),error.message,true);}
    finally{running=false;command.readOnly=false;}
  };
  command.onkeydown=e=>{
    if(e.isComposing)return;
    if(e.key==='Escape'){command.blur();reference(false);return;}
    if(!['ArrowUp','ArrowDown'].includes(e.key)||!history.length)return;
    e.preventDefault();if(cursor===history.length)draft=command.value;
    cursor=Math.min(history.length,Math.max(0,cursor+(e.key==='ArrowUp'?-1:1)));
    command.value=cursor===history.length?draft:history[cursor];
  };
  document.addEventListener('keydown',e=>{
    const chord=(e.ctrlKey||e.metaKey)&&e.shiftKey&&e.key.toLowerCase()==='k';
    if(e.defaultPrevented||e.altKey||e.isComposing||document.querySelector('dialog[open]')||$('pendantEnabled')?.checked)return;
    if(!chord && (e.key!=='/'||e.ctrlKey||e.metaKey||e.target.closest?.('input,select,textarea,[contenteditable=true]')))return;
    e.preventDefault();focusCommand();
  });
  window.addEventListener('cnc-project-loaded',()=>{epoch++;active=null;clearPreview();dialog.close();command.value='';tell($('cad-command-result'),'');});
  function focusCommand(){
    if(document.querySelector('dialog[open]')||$('pendantEnabled')?.checked)return;
    command.focus();command.select();
  }
  window.KerfKiteCAD=Object.freeze({openSelected,openFile,focusCommand,runMacro,execute});
})();
