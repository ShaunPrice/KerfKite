'use strict';
const $=id=>document.getElementById(id), NS='http://www.w3.org/2000/svg';
const offline=location.protocol==='file:';
let webMode=false;
let objects=[], selected=-1, preview=null, previewProject=null, token='', latest={};
let previewEpoch=0;
const fields=['stock_thickness','operation','tool_diameter','stepover','kerf','interval','hatch_angle','crosshatch','v_angle','v_resolution','knife_offset','knife_lead','dwell_units','protocol','width','height','mode','feed','power','depth','step','safe','plunge','passes','lead_in','lead_out','start_power','end_power','ramp_in','ramp_out','lead_shape','laser_pattern','laser_dash','laser_gap','laser_bridge_count','drill_peck','drill_point_angle','router_entry','entry_angle','entry_radius','pen_lift','pen_dwell','pen_up_power','pen_nib','pen_nib_angle','pen_tip'];
function message(text,error=false){$('message').textContent=text;$('message').className=error?'error':'';}
async function api(path,data){const tabProject=data?.project||data;const needsEntry=op=>op?.mode==='router'&&op.router_entry&&op.router_entry!=='plunge';if(!offline&&['generate','arm','run','rotary','tool-changes'].includes(path)&&(needsEntry(tabProject)||tabProject?.operations?.some(op=>needsEntry({...tabProject,...op})))&&!window.studioSession?.router_entries)throw Error('Restart Studio before posting ramp or helical entries with this editor.');const patterned=op=>op?.mode==='laser'&&(op.laser_pattern&&op.laser_pattern!=='continuous'||op.lead_shape&&op.lead_shape!=='straight'&&(op.lead_in||op.lead_out));if(!offline&&['generate','arm','run','rotary','tool-changes'].includes(path)&&(patterned(tabProject)||tabProject?.operations?.some(op=>patterned({...tabProject,...op})))&&!window.studioSession?.laser_patterns)throw Error('Restart Studio and reload before using laser cut patterns and curved leads.');const needsTabs=tabProject?.tabs||tabProject?.operations?.some(op=>op?.tabs);if(!offline&&needsTabs&&['generate','rotary','tool-changes'].includes(path)&&!window.studioSession?.holding_tabs)throw Error('Restart CNC Studio and reload to enable holding tabs; this server cannot preserve them.');if(path==='generate'&&!offline&&(data?.operation&&data.operation!=='line'||data?.operations||data?.path_powers)&&!window.studioSession?.advanced_cam)throw Error('Restart CNC Studio and reload to enable the new CAM operations');if(path==='generate'&&data?.path_depths&&!offline&&!window.studioSession?.depth_paths)throw Error('Restart CNC Studio and reload the editor to enable the new varying-depth CAM');if(offline){if(path==='simulate')return CNCOffline.simulate(data);if(path==='rotary')return CNCRotary.generate(data.project,data.options);if(path==='generate')return CNCOffline.generate(data);if(path==='session')return {token:'',offline:true,depth_paths:true};if(path==='profiles')return [{id:'offline',name:'Local file editor',width:750,height:400,firmware:'Offline design'}];if(path==='ports')return [];throw Error('This advanced design operation requires the app server; basic CAM and G-code work offline.');}
const packedStock=path==='simulate' && window.studioSession?.stock_wire_v1;
const body=packedStock?KerfKiteStockWire.encode(data):(data===undefined?undefined:JSON.stringify(data));
const r=await fetch('/api/'+path,{method:data===undefined?'GET':'POST',headers:{'Content-Type':packedStock?KerfKiteStockWire.mime:'application/json','X-Studio-Token':token},body});const result=await r.json();if(!r.ok)throw Error(result.error||'Request failed');return result;}
function task(fn){return async()=>{try{await fn();}catch(e){message(e.message,true);}};}
function editorSettings(){const p={};for(const k of fields)p[k]=['mode','protocol','operation','dwell_units','lead_shape','laser_pattern','router_entry','pen_lift'].includes(k)?$(k).value:Number($(k).value);return p;}
function project(){const p=editorSettings();p.paths=objects.flatMap(o=>o.paths);if(p.mode==='laser'&&objects.some(o=>o.powers))p.path_powers=objects.flatMap(o=>o.paths.map((path,i)=>o.powers?.[i]?.map(v=>v*p.power)||path.map(()=>p.power)));if(p.mode==='router'&&objects.some(o=>o.depths))p.path_depths=objects.flatMap(o=>o.paths.map((path,i)=>o.depths?.[i]||path.map(()=>-p.depth)));return p;}
function machiningProject(){window.CNCSolidLinks?.requireCurrent();return project();}
function invalidate(){previewEpoch++;window.stockPreview=null;window.recordHistory?.();preview=null;previewProject=null;$('confirmed').checked=false;$('gcode').textContent='Generate a new preview after editing.';$('summary').textContent=`${objects.length} objects · preview needs generating`;$('playhead').replaceChildren();$('toolpaths').replaceChildren();cancelPhoneArm();}
function drawingPath(paths,height){
 return paths.map(path=>'M'+path.map(([x,y])=>`${x},${height-y}`).join(' L')).join(' ');
}
let objectFrame=0;
function repaintActiveObject(){
 if(objectFrame)return;
 objectFrame=requestAnimationFrame(()=>{
   objectFrame=0;
   const o=objects[selected],node=$('drawing').children[selected];
   if(!node||!o)return;
   node.setAttribute('d',drawingPath(o.paths,Number($('height').value)));
   renderSelection();
 });
}
function draw(){if(objectFrame){cancelAnimationFrame(objectFrame);objectFrame=0;}const w=Number($('width').value)||300,h=Number($('height').value)||180;$('canvas').setAttribute('viewBox',window.KerfKiteViews?.drawingBox(w,h) || `-12 -12 ${w+24} ${h+24}`);$('stock').setAttribute('width',w);$('stock').setAttribute('height',h);$('dimensions').textContent=`${w} × ${h} mm`;$('drawing').replaceChildren();$('objects').replaceChildren();objects.forEach((o,i)=>{const g=document.createElementNS(NS,'path');g.setAttribute('d',drawingPath(o.paths,h));g.setAttribute('fill','none');g.setAttribute('stroke',i===selected?'#c77829':'#176a9a');g.setAttribute('stroke-width',i===selected?'2.5':'1.5');g.dataset.object=i;g.style.pointerEvents='bounding-box';$('drawing').append(g);const b=document.createElement('button');b.className='object'+(i===selected?' selected':'');b.textContent=`${i+1}. ${o.output===false?'[OFF] ':''}${o.name}${o.operationSettings?' · '+o.operationSettings.operation:''}`;b.onclick=()=>{selected=i;draw();};$('objects').append(b);});renderSelection();}
function add(name,paths,properties={}){objects.push({designLayerId:window.CNCDesignLayers?.activeId?.()||'default',...properties,name,paths});selected=objects.length-1;invalidate();draw();}
function shape(){const a=['shapeX','shapeY','shapeW','shapeH'].map(k=>Number($(k).value));if(a.some(x=>!Number.isFinite(x))||a[2]<=0||a[3]<=0)throw Error('Enter a positive shape width and height');return a;}
function insertPrimitive(kind,x,y,w,h){
 if(kind==='rectangle')add('Rectangle',[[[x,y],[x+w,y],[x+w,y+h],[x,y+h],[x,y]]]);
 else if(kind==='ellipse')add('Ellipse',[Array.from({length:129},(_,i)=>[x+w/2+Math.cos(i*Math.PI/64)*w/2,y+h/2+Math.sin(i*Math.PI/64)*h/2])]);
 else if(kind==='line')add('Line',[[[x,y],[x+w,y+h]]]);
 else throw Error('Unknown shape');
}
for(const [id,kind] of [['rect','rectangle'],['ellipse','ellipse'],['line','line']])$(id).onclick=task(()=>insertPrimitive(kind,...shape()));
function bounds(paths){
 let x=Infinity,y=Infinity,right=-Infinity,top=-Infinity;
 for(const path of paths)for(const [px,py] of path){
   x=Math.min(x,px);y=Math.min(y,py);right=Math.max(right,px);top=Math.max(top,py);
 }
 return [x,y,right,top];
}
$('move').onclick=task(()=>{if(selected<0)throw Error('Select an object');const[x,y]=shape(),o=objects[selected],[bx,by]=bounds(o.paths);o.paths=o.paths.map(p=>p.map(([px,py])=>[px-bx+x,py-by+y]));invalidate();draw();});
$('scale').onclick=task(()=>{if(selected<0)throw Error('Select an object');const[,,w,h]=shape(),o=objects[selected];if(o.gear)throw Error('Change the selected gear module in Properties to resize it');const[x,y,mx,my]=bounds(o.paths);o.paths=o.paths.map(p=>p.map(([px,py])=>[x+(px-x)*w/(mx-x||1),y+(py-y)*h/(my-y||1)]));invalidate();draw();});
$('remove').onclick=()=>{if(selected>=0)objects.splice(selected,1);selected=-1;invalidate();draw();};
function download(name,text,type='text/plain'){const u=URL.createObjectURL(new Blob([text],{type})),a=document.createElement('a');a.href=u;a.download=name;a.click();setTimeout(()=>URL.revokeObjectURL(u),1000);}
$('saveProject').onclick=()=>download('design.cncstudio.json',JSON.stringify({format:'pi-cnc-studio/1',settings:project(),objects},null,2),'application/json');
let fileMode='svg';$('svgImport').onclick=()=>{fileMode='svg';$('file').accept='.svg';$('file').click();};$('loadProject').onclick=()=>{fileMode='project';$('file').accept='.json';$('file').click();};
async function loadFile(file,kind){if(!file)return;if(file.size>4000000)throw Error('File exceeds 4 MB');const text=await file.text();if(kind==='project'){const d=JSON.parse(text);if(d.format!=='pi-cnc-studio/1'||!Array.isArray(d.objects)||d.objects.some(o=>!Array.isArray(o.paths)||typeof o.name!=='string'))throw Error('Invalid project file');const ps=d.objects.flatMap(o=>o.paths);if(ps.length>3000||ps.flat().length>100000||ps.some(p=>!Array.isArray(p)||p.length<2||p.some(pt=>!Array.isArray(pt)||pt.length!==2||pt.some(n=>typeof n!=='number'||!Number.isFinite(n)||Math.abs(n)>2000))))throw Error('Invalid project geometry');for(const o of d.objects)for(const[key,lo,hi]of[['depths',-50,0],['powers',0,1]])if(o[key]!==undefined&&(!Array.isArray(o[key])||o[key].length!==o.paths.length||o[key].some((values,i)=>!Array.isArray(values)||values.length!==o.paths[i].length||values.some(v=>typeof v!=='number'||!Number.isFinite(v)||v<lo||v>hi))))throw Error('Invalid vertex '+key);const hasMachining=ps.length>0 && (!Array.isArray(d.settings?.operations)||d.settings.operations.length>0);if(!offline&&hasMachining)await api('generate',{...d.settings,paths:d.objects.flatMap(o=>o.paths),path_depths:d.settings.mode==='router'&&d.objects.some(o=>o.depths)?d.objects.flatMap(o=>o.paths.map((path,i)=>o.depths?.[i]||path.map(()=>-d.settings.depth))):undefined});for(const k of fields){if(d.settings[k]!==undefined)$(k).value=d.settings[k];else if(['lead_in','lead_out','ramp_in','ramp_out'].includes(k))$(k).value=0;else if(['start_power','end_power'].includes(k))$(k).value=100;else if(k==='dwell_units')$(k).value='';else if(k==='operation')$(k).value='line';else if(k==='drill_peck')$(k).value=0;else if(k==='drill_point_angle')$(k).value=118;else if(k==='router_entry')$(k).value='plunge';else if(k==='entry_angle')$(k).value=3;else if(k==='entry_radius')$(k).value=1;else if(Object.hasOwn(laserFieldDefaults,k))$(k).value=laserFieldDefaults[k];}objects=d.objects;selected=-1;modeUI();invalidate();draw();message('Project opened');}else{importSVG(text,file.name);}}
$('file').onchange=task(async()=>{const file=$('file').files[0];$('file').value='';await loadFile(file,fileMode);});
function importSVG(text,name){
  const paths=KerfKiteSvg.parse(text,{box:shape(),physical:$('svgSizing').value==='physical',
    bed:[Number($('width').value),Number($('height').value)]});
  add(name,paths);
  message('SVG imported with '+paths.length+' separate '+(paths.length===1?'path':'paths')+'.');
}
const laserFieldDefaults={lead_shape:'straight',laser_pattern:'continuous',laser_dash:3,laser_gap:1,laser_bridge_count:4};
function showLaserPattern(){
  const pattern=$('laser_pattern').value;
  $('laserDashLabel').hidden=pattern!=='perforate';
  $('laserBridgeLabel').hidden=pattern!=='bridges';
  $('laserGapLabel').hidden=pattern==='continuous';
  $('laserPatternHint').hidden=pattern==='continuous';
  $('laserPatternHint').textContent=pattern==='bridges'?'Equal beam-off gaps are centred around each closed contour. Check holes and neighbouring paths in the preview.':'Cut and beam-off lengths repeat from the start of each contour. Leads remain continuous.';
}
function showRouterEntry(){
  const entry=$('router_entry').value;
  $('routerEntryPanel').hidden=$('mode').value!=='router';
  $('entryAngleLabel').hidden=entry==='plunge';
  $('entryRadiusLabel').hidden=entry!=='helix';
  $('entryHint').hidden=entry==='plunge';
  $('entryHint').textContent=entry==='helix'?'Pocket only. Enter a centreline radius up to half the cutter diameter. Studio checks the spiral and the feed to each contour against pocket boundaries and islands.':'Use a flat end mill. Ramp over complete laps of each closed contour, then finish one lap at the pass depth.';
}
function modeUI(){showRouterEntry();showLaserPattern();$('drillPanel').hidden=$('mode').value!=='router';const laser=$('mode').value==='laser',pen=$('mode').value==='pen',servo=pen&&$('pen_lift').value==='servo';$('routerSettings').hidden=laser||servo;$('laserSettings').hidden=!laser;$('penSettings').hidden=!pen;$('penDwellLabel').hidden=$('penUpLabel').hidden=!servo;$('depthLabel').firstChild.textContent=pen?'Pen pressed below Z zero · mm':'Depth · mm';$('safeLabel').firstChild.textContent=pen?'Pen lift · mm':'Clearance Z · mm';$('powerLabel').firstChild.textContent=laser?'Laser S value':servo?'Pen-down S value':pen?'Spindle RPM · not used':'Spindle RPM';$('modeHint').textContent=laser?'Requires laser mode on the controller. S value must match your configured maximum; it is not a percentage.':servo?'The pen is lifted from the spindle or laser output and Z is not moved. Check which S values lift and lower the holder before putting a pen in.':pen?'The spindle stays off. Z zero is where the pen touches the paper; the depth is how far the spring is pressed.':'Z zero is the stock surface. Paths follow the cutter centre; this is not variable-depth V-carving.';}
for(const k of fields)$(k).oninput=()=>{modeUI();invalidate();draw();};
async function generateDesignPreview(){invalidate();const epoch=previewEpoch,p=machiningProject();const r=await api('generate',p);if(epoch!==previewEpoch||JSON.stringify(p)!==JSON.stringify(machiningProject()))throw Error('Design changed during generation; preview again');preview=r;previewProject=JSON.stringify(p);$('gcode').textContent=r.lines.join('\n');$('summary').textContent=`${r.lines.length} lines · ${r.cut_length} mm cutting · ${r.cut_minutes} min cutting only. ${r.warnings.join(' ')}`;message(CNCProtocols.get(p.protocol).live_transport?'Preview ready. Confirm setup before running.':'Preview ready. Export G-code and load it in the selected controller software.');renderToolpaths();scrub();window.render3D?.();}
$('preview').onclick=task(generateDesignPreview);
$('export').onclick=task(()=>{if(!preview||previewProject!==JSON.stringify(machiningProject()))throw Error('Generate a current preview first');download('engraving.nc',preview.lines.join('\n')+'\n');});
function scrub(){if(!preview)return;const n=Math.max(1,Math.ceil(preview.segments.length*Number($('scrub').value)/100));const s=preview.segments[n-1],circle=document.createElementNS(NS,'circle');circle.setAttribute('cx',s[2]);circle.setAttribute('cy',Number($('height').value)-s[3]);circle.setAttribute('r','1.7');circle.setAttribute('fill','#c77829');$('playhead').replaceChildren(circle);}$('scrub').oninput=scrub;
$('connect').onclick=task(async()=>{await api('connect',{port:$('port').value,protocol:$('protocol').value});message('Connection requested');});$('disconnect').onclick=task(()=>api('disconnect',{}));
$('run').onclick=task(async()=>{if(!preview||previewProject!==JSON.stringify(machiningProject()))throw Error('Generate a current preview first');if(!$('confirmed').checked)throw Error('Confirm machine setup first');const p=machiningProject();await api('run',{project:p,...reviewedRunOptions(p),...(window.KerfKiteLeveling?.runOptions()||{}),confirmed:true});$('confirmed').checked=false;message('Job submitted; monitor the controller status');});
for(const b of document.querySelectorAll('[data-command]'))b.onclick=task(async()=>{const action=b.dataset.command;if(['home','zero','unlock'].includes(action)&&!confirm(`${b.textContent} on the connected controller?`))return;await api('command',{action});});
for(const b of document.querySelectorAll('[data-jog]'))b.onclick=task(()=>{const[axis,sign]=b.dataset.jog.split(',');return api('command',{action:'jog',axis,distance:Number(sign)*Number($('jogDistance').value),feed:Number($('jogFeed').value)});});
$('demo').onclick=()=>{objects=[{name:'Workshop badge',paths:[[[30,30],[130,30],[130,90],[30,90],[30,30]],[[40,40],[120,40],[120,80],[40,80],[40,40]]]},{name:'Diamond',paths:[[[80,48],[105,60],[80,72],[55,60],[80,48]]]}];selected=0;invalidate();draw();};
async function poll(){
  try {
    latest=await api('status');
    $('state').textContent=(latest.simulated?'SIMULATOR · ':'')+(latest.state||'Disconnected')+' · '+(latest.protocol||'grbl');
    $('progress').value=latest.progress||0;
    $('log').textContent=(latest.log||[]).join('\n');
    $('position').textContent=latest.position?JSON.stringify(latest.position):'Position unavailable';
    $('run').disabled=!latest.connected||!CNCProtocols.get($('protocol').value).live_transport||latest.protocol!==$('protocol').value;
    const holdButton=document.querySelector('[data-command="hold"]');
    const resumeButton=document.querySelector('[data-command="resume"]');
    holdButton.disabled=!latest.connected||['Hold requested','Decelerating','Hold'].includes(latest.state);
    resumeButton.disabled=!latest.connected||latest.state!=='Hold'||!!latest.error;
    if(latest.error)message(latest.error,true);
    await checkPhoneArm();
  } catch(e) {
    $('state').textContent='Server unavailable';
    $('run').disabled=true;
    document.querySelector('[data-command="resume"]').disabled=true;
    cancelPhoneArm();
  }
  setTimeout(poll,700);
}
window.studioReady=(async()=>{try{const session=await api('session');token=session.token;webMode=!!session.web_mode;window.studioSession=session;const profiles=await api('profiles');for(const p of profiles){const o=document.createElement('option');o.value=p.id;o.textContent=p.name;$('profile').append(o);}$('profile').onchange=()=>{const p=profiles.find(p=>p.id===$('profile').value);$('width').value=p.width;$('height').value=p.height;$('profileHint').textContent=p.firmware+(session.hardware_enabled?' · physical ports enabled; commissioning required':' · simulator only; hardware commissioning pending')+(p.travel_conflict?' · '+p.travel_conflict:'');invalidate();draw();};$('profile').onchange();for(const p of (webMode?[]:await api('ports'))){const option=document.createElement('option');option.value=p.device;option.textContent=p.device+' · '+p.description;$('port').append(option);}draw();if(!offline&&!webMode)poll();else{ $('machinePanel').hidden=true;for(const id of ['connect','disconnect','port'])$(id).hidden=true;$('state').textContent=offline?'LOCAL FILE EDITOR':'HTTPS EDITOR · '+session.username;$('profileHint').textContent='Design and file preparation; machine inbox uploads do not start motion';}return session;}catch(e){message(e.message,true);throw e;}})();

function svgNode(tag,attrs){const n=document.createElementNS(NS,tag);for(const[k,v]of Object.entries(attrs))n.setAttribute(k,v);return n;}
function renderSelection(){const layer=$('selection');layer.replaceChildren();if(selected<0||!objects[selected])return;const[x,y,mx,my]=bounds(objects[selected].paths),h=Number($('height').value),matrix=$('canvas').getScreenCTM(),size=matrix?10/Math.abs(matrix.a):4;layer.append(svgNode('rect',{x,y:h-my,width:mx-x,height:my-y,class:'outline'}));for(const[handle,hx,hy,cursor]of[['sw',x,y,'nesw-resize'],['s',(x+mx)/2,y,'ns-resize'],['se',mx,y,'nwse-resize'],['e',mx,(y+my)/2,'ew-resize'],['ne',mx,my,'nesw-resize'],['n',(x+mx)/2,my,'ns-resize'],['nw',x,my,'nwse-resize'],['w',x,(y+my)/2,'ew-resize']]){const node=svgNode('rect',{x:hx-size/2,y:h-hy-size/2,width:size,height:size,class:'handle','data-handle':handle});node.style.cursor=cursor;layer.append(node);}}
function canvasPoint(event){const p=new DOMPoint(event.clientX,event.clientY).matrixTransform($('canvas').getScreenCTM().inverse());return[p.x,Number($('height').value)-p.y];}
let gesture=null;
$('canvas').addEventListener('pointerdown',e=>{
 if(e.button!==0||gesture)return;
 const handle=e.target.dataset.handle,index=e.target.dataset.object;
 if(!handle&&index===undefined){selected=-1;draw();return;}
 if(index!==undefined)selected=Number(index);
 const o=objects[selected];if(!o)return;
 if(handle&&o.gear){message('Change the selected gear module in Properties to resize it',true);return;}
 const placement=window.KerfPlacement,indices=!handle&&placement?placement.members(objects,selected):[selected];
 const box=placement?placement.extent(objects,indices):bounds(o.paths), start=canvasPoint(e);
 const candidates=placement&&$('placement-vertices').checked?placement.candidates(indices):[];
 const ctm=$('canvas').getScreenCTM();
 const anchor=placement?placement.nearest(start,o.paths.flat(),8/Math.max(.001,Math.hypot(ctm.a,ctm.b))):start;
 gesture={id:e.pointerId,start,anchor,original:o.paths,originals:objects.slice(),indices,box,candidates,handle,index:selected,moved:false};
 $('canvas').setPointerCapture(e.pointerId);draw();
 if(handle){
   gesture.handles=Array.from($('selection').querySelectorAll('rect.handle,circle'),node=>{
     const circle=node.localName==='circle';
     return {node,x:circle?node.cx.baseVal.value:node.x.baseVal.value+node.width.baseVal.value/2,
       y:circle?node.cy.baseVal.value:node.y.baseVal.value+node.height.baseVal.value/2};
   });
 }
 e.preventDefault();
});
$('canvas').addEventListener('pointermove',e=>{
 if(!gesture||e.pointerId!==gesture.id)return;
 const p=canvasPoint(e),g=gesture,[x,y,mx,my]=g.box,w=Number($('width').value),h=Number($('height').value);
 const snap=(point)=>window.KerfPlacement?.snap(point,g.candidates,e.altKey)||point;
 if(!g.moved&&Math.hypot(p[0]-g.start[0],p[1]-g.start[1])<1e-6)return;
 if(!g.moved){invalidate();g.moved=true;}
 if(!g.handle){
   const target=snap(p.map((v,i)=>g.anchor[i]+v-g.start[i]));
   g.dx=Math.max(-x,Math.min(w-mx,target[0]-g.anchor[0]));
   g.dy=Math.max(-y,Math.min(h-my,target[1]-g.anchor[1]));
   g.transform=`translate(${g.dx} ${-g.dy})`;
 }else{
   const [px,py]=snap(p);let nx=x,ny=y,nmx=mx,nmy=my;
   if(g.handle.includes('w'))nx=Math.max(0,Math.min(mx-.1,px));
   if(g.handle.includes('e'))nmx=Math.min(w,Math.max(x+.1,px));
   if(g.handle.includes('s'))ny=Math.max(0,Math.min(my-.1,py));
   if(g.handle.includes('n'))nmy=Math.min(h,Math.max(y+.1,py));
   g.sx=mx===x?1:(nmx-nx)/(mx-x);g.sy=my===y?1:(nmy-ny)/(my-y);
   g.tx=mx===x?0:nx-x*g.sx;g.ty=my===y?0:ny-y*g.sy;
   g.transform=`matrix(${g.sx} 0 0 ${g.sy} ${g.tx} ${h*(1-g.sy)-g.ty})`;
 }
 // Keep the original vertices and group metadata until release. A drag frame
 // touches only the selected SVG paths and overlay, regardless of drawing size.
 if(!g.frame)g.frame=requestAnimationFrame(()=>{
   g.frame=0;
   if(gesture!==g)return;
   for(const index of g.indices)$('drawing').children[index]?.setAttribute('transform',g.transform);
   $('selection').setAttribute('transform',g.transform);
   if(g.handle)for(const {node,x,y} of g.handles){
     // The overlay follows the scaled geometry, while handles retain their screen size.
     node.setAttribute('transform',`translate(${x} ${y}) scale(${1/g.sx} ${1/g.sy}) translate(${-x} ${-y})`);
   }
 });
 e.preventDefault();
});
function endGesture(cancel=false){
 if(!gesture)return;const g=gesture;gesture=null;
 if(g.frame)cancelAnimationFrame(g.frame);
 $('selection').removeAttribute('transform');
 if($('canvas').hasPointerCapture(g.id))$('canvas').releasePointerCapture(g.id);
 if(!cancel&&g.moved&&objects.length===g.originals.length&&g.indices.every(i=>objects[i]===g.originals[i])){
   if(g.handle){
     const paths=g.original.map(path=>path.map(([x,y])=>[x*g.sx+g.tx,y*g.sy+g.ty]));
     objects[g.index]={...objects[g.index],paths};
   }else objects=CNCLayout.apply(objects,g.indices.map(index=>({index,dx:g.dx,dy:g.dy})));
   const[x,y,mx,my]=bounds(objects[g.index].paths);
   for(const[id,value]of [['shapeX',x],['shapeY',y],['shapeW',mx-x],['shapeH',my-y]])$(id).value=value.toFixed(2);
   invalidate();
 }
 draw();
}
$('canvas').addEventListener('pointerup',e=>{if(e.pointerId===gesture?.id)endGesture();});
for(const type of ['pointercancel','lostpointercapture'])$('canvas').addEventListener(type,e=>{if(e.pointerId===gesture?.id)endGesture(true);});
window.addEventListener('keydown',e=>{if(e.key==='Escape')endGesture(true);});
window.addEventListener('blur',()=>endGesture(true));window.addEventListener('resize',renderSelection);
for(const id of ['rect','ellipse','line','polygon','gear'])$(id).addEventListener('dragstart',e=>{e.dataTransfer.setData('application/x-cnc-shape',id);e.dataTransfer.effectAllowed='copy';});
const bed=document.querySelector('.bed');bed.addEventListener('dragover',e=>{e.preventDefault();bed.classList.add('drop-target');e.dataTransfer.dropEffect='copy';});bed.addEventListener('dragleave',e=>{if(!bed.contains(e.relatedTarget))bed.classList.remove('drop-target');});bed.addEventListener('drop',async e=>{e.preventDefault();bed.classList.remove('drop-target');try{const id=e.dataTransfer.getData('application/x-cnc-shape');if(['rect','ellipse','line','polygon','gear'].includes(id)){const[x,y]=canvasPoint(e);$('shapeX').value=Math.max(0,Math.min(Number($('width').value)-Number($(id==='gear'?'gearDiameter':'shapeW').value),x)).toFixed(2);$('shapeY').value=Math.max(0,Math.min(Number($('height').value)-Number($(id==='gear'?'gearDiameter':'shapeH').value),y)).toFixed(2);$(id).click();}else{const files=[...e.dataTransfer.files];if(files.length!==1)throw Error('Drop one SVG or project file at a time');const file=files[0];if(window.CNCInterchange?.recognises(file)){await CNCInterchange.open(file);return;}if(!/\.(svg|json)$/i.test(file.name))throw Error('Drop an SVG or .cncstudio.json project');await loadFile(file,/\.json$/i.test(file.name)?'project':'svg');}}catch(error){message(error.message,true);}});
function renderToolpaths(){const layer=$('toolpaths');layer.replaceChildren();if(!preview)return;const h=Number($('height').value);if(preview.drill_holes){for(const [x,y,diameter] of preview.drill_holes)layer.append(svgNode('circle',{cx:x,cy:h-y,r:diameter/2,fill:'none',stroke:'#116d80','stroke-width':2,'vector-effect':'non-scaling-stroke'}));}const groups=new Map();for(const s of preview.segments){if(preview.mode==='laser'&&s[5]===0)continue;const ratio=s[5]===null?1:s[5]/Number($('power').value),bin=Math.max(0,Math.min(10,Math.round(ratio*10)));groups.set(bin,(groups.get(bin)||'')+`M${s[0]},${h-s[1]}L${s[2]},${h-s[3]} `);}for(const[bin,d]of groups){layer.append(svgNode('path',{d,fill:'none',stroke:`hsl(${210-bin*16} 75% 45%)`,'stroke-width':6,'stroke-opacity':.55,'vector-effect':'non-scaling-stroke'}));}}
// Only this live document receives the handle for the preview that it armed.
let phoneArm=null, armInFlight=false, pageGeneration=0;
const disarms=new Set(), sendingDisarms=new Set();
const armNotice=document.createElement('p');
armNotice.className='hint';
armNotice.setAttribute('role','status');
$('armPhone').after(armNotice);

function setArmNotice(text,error=false){
  armNotice.textContent=text;
  if(error)message(text,true);
}
async function sendDisarm(id){
  if(sendingDisarms.has(id))return;
  sendingDisarms.add(id);
  try {
    await api('disarm',{arm_id:id});
    disarms.delete(id);
    if(!phoneArm&&!disarms.size)setArmNotice('Phone Start is disarmed.');
  } catch(error) {
    if(!phoneArm)setArmNotice('Preview invalidated. Waiting to confirm disarm with the workstation server.',true);
  } finally {
    sendingDisarms.delete(id);
  }
}
function cancelPhoneArm(){
  if(!phoneArm)return;
  const id=phoneArm.id;
  phoneArm=null;
  disarms.add(id);
  setArmNotice('Preview invalidated. Confirming disarm…');
  void sendDisarm(id);
}
function reviewedRunOptions(project){
  if(!preview?.rotary)return {};
  if(!window.KerfKiteRotary)throw Error('Reload the rotary editor and generate a new wrapped preview.');
  return KerfKiteRotary.request(project,preview);
}
function sameArmedPreview(arm, checkDocument=true){
  return phoneArm===arm && arm.generation===pageGeneration && preview===arm.preview && previewProject===arm.document
    && (!checkDocument||JSON.stringify(machiningProject())===arm.document && (!preview.rotary||KerfKiteRotary.current(machiningProject(),preview)));
}
async function checkPhoneArm(){
  for(const id of disarms)void sendDisarm(id);
  const arm=phoneArm;
  if(!arm)return;
  try {
    if(!sameArmedPreview(arm,false)){cancelPhoneArm();return;}
    const result=await api('arm/check',{arm_id:arm.id});
    if(phoneArm!==arm)return;
    if(!result.armed){
      phoneArm=null;
      setArmNotice('This preview is no longer armed. Check controller status before arming again.');
      return;
    }
    if(result.challenge){
      if(!sameArmedPreview(arm)){cancelPhoneArm();return;}
      setArmNotice('Phone Start requested. Confirming the current preview…');
      await api('arm/confirm',{arm_id:arm.id,challenge:result.challenge});
      if(phoneArm===arm)setArmNotice('Preview confirmed for this Start request. Check controller status.');
    }
  } catch(error) {
    if(phoneArm===arm){cancelPhoneArm();setArmNotice('Phone Start confirmation failed. Check controller status and rearm the preview.',true);}
  }
}
window.addEventListener('pagehide',()=>{
  pageGeneration++;
  if(!phoneArm)return;
  const id=phoneArm.id;
  phoneArm=null;
  fetch('/api/disarm',{method:'POST',keepalive:true,
    headers:{'Content-Type':'application/json','X-Studio-Token':token},body:JSON.stringify({arm_id:id})}).catch(()=>{});
});
$('armPhone').onclick=task(async()=>{
  if(!window.studioSession?.phone_arm_challenge)throw Error('Update the workstation server and reload this editor before arming phone Start.');
  if(armInFlight)throw Error('An arm request is already in progress.');
  if(!preview||previewProject!==JSON.stringify(machiningProject()))throw Error('Generate a current preview first');
  if(!$('confirmed').checked)throw Error('Confirm setup first');
  window.KerfKiteLeveling?.requireWorkstationStart();
  const project=machiningProject(),snapshot=preview,document=previewProject,generation=pageGeneration;
  armInFlight=true;
  cancelPhoneArm();
  try {
    const result=await api('arm',{project,...reviewedRunOptions(project),confirmed:true,phone_arm_challenge:true});
    if(typeof result.arm_id!=='string'||!result.arm_id)throw Error('The server did not supply an arm handle. Update the server before using phone Start.');
    phoneArm={id:result.arm_id,preview:snapshot,document,generation};
    if(!sameArmedPreview(phoneArm)){
      cancelPhoneArm();
      throw Error('Design changed. Generate and arm a fresh preview.');
    }
    $('confirmed').checked=false;
    setArmNotice('Preview armed. Keep this workstation tab open and connected for phone Start.');
    message(armNotice.textContent);
  } finally {armInFlight=false;}
});

function addGenerated(name,paths,properties){const[x,y,mx,my]=bounds(paths);if(x<0||y<0||mx>Number($('width').value)||my>Number($('height').value))throw Error('Shape exceeds the work area; reduce its size or change X/Y');add(name,paths,properties);}
$('polygon').onclick=task(()=>{const[x,y,width,height]=shape(),sides=Number($('polygonSides').value),options={x,y,width,height,sides,bore:Number($('polygonBore').value),rotation:Number($('shapeRotation').value)};addGenerated(`${sides}-sided polygon`,CNCShapes.polygon(options));objects[selected].polygon=options;draw();});
function gearOptions(){return {x:$('shapeX').value,y:$('shapeY').value,teeth:$('gearTeeth').value,module:$('gearModule').value,pressureAngle:$('gearPressure').value,backlash:$('gearBacklash').value,bore:$('gearBore').value,rotation:$('shapeRotation').value};}
function gearSize(){const m=Number($('gearModule').value),z=Number($('gearTeeth').value);$('gearDiameter').value=(m*(z+2)).toFixed(3);}
for(const id of ['gearTeeth','gearModule'])$(id).addEventListener('input',gearSize);gearSize();
$('gear').onclick=task(()=>{const options=gearOptions();addGenerated(`Involute gear · ${options.teeth} teeth`,CNCShapes.gear(options));objects[selected].gear=options;draw();message('Involute gear added. Use matching module and pressure angle for the mating gear. Cutting requires cutter/kerf compensation in CAM.');});
