/* Surface control nets and retained STEP geometry belong to the portable project. */
(() => {
  'use strict';
  const open = document.createElement('button');
  open.type = 'button';
  open.id = 'cad-exchange-open';
  open.textContent = 'Surfaces and STEP';
  document.querySelector('.solid-tools').append(open);
  const dialog = document.createElement('dialog');
  dialog.className = 'solid-sketch-dialog';
  dialog.setAttribute('aria-labelledby','exchange-title');
  dialog.innerHTML = `
    <div class="solid-top"><h2 id="exchange-title">Surfaces and STEP</h2><button id="exchange-close">Close</button></div>
    <label>Geometry<select id="exchange-kind"><option value="surface">Editable surface patches</option><option value="reference">Imported STEP</option><option value="part">Current solid part</option></select></label>
    <canvas id="exchange-canvas" width="700" height="300" tabindex="0" aria-label="Geometry preview. Drag or use the arrow keys to orbit. Click a control point to edit." style="width:100%;height:260px;background:#193341;border-radius:6px;touch-action:none"></canvas>
    <div id="exchange-net">
      <div id="exchange-sheet-tools">
        <div class="solid-pair"><label>Active patch<select id="exchange-patch"></select></label><label>Join direction<select id="exchange-axis"><option value="u">Along U</option><option value="v">Along V</option></select></label></div>
        <div class="solid-actions"><button id="exchange-append">Join a new patch</button><button id="exchange-remove">Remove last patch</button></div>
        <label id="exchange-seam-label">Seam with previous patch<select id="exchange-continuity"><option value="position">Position (C0)</option><option value="tangent">Match tangent (C1)</option></select></label>
        <details><summary>Trim surface bounds</summary>
          <div class="solid-pair">${[['u0','U start'],['u1','U end'],['v0','V start'],['v1','V end']].map(([id,label])=>`<label>${label}<input id="exchange-${id}" type="number" min="0" max="1" step="any"></label>`).join('')}</div>
          <p class="hint">0 to 1 spans the whole surface in each direction. Patches outside the retained bounds stay editable.</p>
          <div class="solid-actions"><button id="exchange-trim">Apply trim</button><button id="exchange-untrim">Reset bounds</button><button id="exchange-outline">Draw trim outline or holes</button></div>
        </details>
      </div>
      <label>Surface basis<select id="exchange-basis"><option value="bezier">Bézier</option><option value="bspline">Clamped B-spline / NURBS</option></select></label>
      <div class="solid-pair" id="exchange-degrees"><label>U degree<input id="exchange-du" type="number" min="1" max="7"></label><label>V degree<input id="exchange-dv" type="number" min="1" max="7"></label></div>
      <p class="hint" id="exchange-driven" hidden>This control point follows the previous patch at the seam. Edit the previous patch to move it.</p>
      <label>Control point<select id="exchange-point"></select></label>
      <div class="solid-triple">${['X','Y','Z'].map(a=>`<label>${a} · mm<input id="exchange-${a}" type="number" step="any" min="-100000" max="100000"></label>`).join('')}</div>
      <label>Point weight<input id="exchange-weight" type="number" min=".001" max="1000" step="any"></label>
      <div class="solid-actions"><button id="exchange-apply">Apply point</button><button id="exchange-row">Extend U</button><button id="exchange-column">Extend V</button><button id="exchange-guide">Practice shaping a patch</button></div>
    </div>
    <div id="exchange-lesson" hidden><p id="exchange-instruction"></p><button id="exchange-lesson-exit">Exit practice</button><button id="exchange-lesson-keep" hidden>Keep practice surface</button></div>
    <p id="exchange-status" role="status"></p>
    <div class="solid-actions"><button id="exchange-import">Import STEP…</button><input id="exchange-file" type="file" accept=".step,.stp" hidden>
      <button id="exchange-export">Export STEP</button><button id="exchange-add">Add STEP to part</button><button id="exchange-relief">Prepare router relief</button></div>`;
  document.body.append(dialog);

  const outlineDialog=document.createElement('dialog');
  outlineDialog.className='solid-sketch-dialog';
  outlineDialog.setAttribute('aria-labelledby','outline-heading');
  outlineDialog.innerHTML=`
    <div class="solid-top"><h2 id="outline-heading">Surface trim outline</h2><button id="outline-cancel">Cancel</button></div>
    <label>Loop<select id="outline-loop"></select></label>
    <svg id="outline-pad" viewBox="0 0 100 100" aria-label="UV trim drawing. Click to add a point, or enter coordinates below." style="display:block;width:min(100%,24rem);margin:auto;background:#e3edeb;border:1px solid #6a8076;touch-action:none"></svg>
    <p class="hint">U increases rightward and V upward. The lower-left corner is 0, 0. Click vertices in order around the outline or hole.</p>
    <label>UV coordinates, one U, V pair per line<textarea id="outline-points" rows="4"></textarea></label>
    <div class="solid-actions"><button id="outline-square">Use unit square</button><button id="outline-back">Remove last point</button>
      <button id="outline-hole">Add hole</button><button id="outline-drop">Remove hole</button></div>
    <p id="outline-status" role="status"></p>
    <div class="solid-actions"><button id="outline-apply">Apply outline</button><button id="outline-clear">Clear outline and holes</button></div>`;
  document.body.append(outlineDialog);

  function saddle(flat = false) {
    return {poles:Array.from({length:4}, (_,i)=>Array.from({length:4}, (_,j)=>
      [20*i, 12*j, flat ? 0 : 3*(i-1.5)*(j-1.5)]))};
  }
  const clone = obj=>JSON.parse(JSON.stringify(obj));
  const selector = $('exchange-kind'), pointList = $('exchange-point'), canvas = $('exchange-canvas');
  let doc, current, revision=0, selected=0, patchIndex=0, lesson=null, valid=true;
  let yaw=-.63, tilt=.62, projectedPoles=[], drag=null;
  const status = text=>{ $('exchange-status').textContent=text; };
  const activeSurface = ()=>lesson ? lesson.surface : doc.surface;
  const activePatch = () => activeSurface().patches?.[patchIndex] || activeSurface();
  const trimFields = ['u0','u1','v0','v1'];
  const fullTrim = [0,1,0,1];

  function validNet(patch) {
    const rows=patch?.poles;
    return Array.isArray(rows) && rows.length>=2 && rows.length<=8 && rows.every(row=>
      Array.isArray(row) && row.length>=2 && row.length<=8 && row.length===rows[0].length &&
      row.every(p=>Array.isArray(p) && p.length===3 && p.every(v=>Number.isFinite(v)&&Math.abs(v)<=100000)));
  }
  function validSurface(surface) {
    if(surface?.kind!=='sheet')return validNet(surface);
    return ['u','v'].includes(surface.axis) && Array.isArray(surface.patches) &&
      surface.patches.length>=2 && surface.patches.length<=16 && surface.patches.every(validNet);
  }
  function store() {
    window.projectExtras.cadExchange=clone(doc);
    window.dispatchEvent(new Event('cnc-project-changed'));
  }
  function enableButtons() {
    const ready=Boolean(current);
    $('exchange-apply').disabled=!valid || offline;
    $('exchange-sheet-tools').hidden=Boolean(lesson);
    $('exchange-guide').disabled=!valid || offline || Boolean(lesson);
    $('exchange-import').disabled=!valid || offline || Boolean(lesson);
    $('exchange-add').hidden=selector.value!=='reference';
    $('exchange-add').disabled=!ready || !current?.solidOnly;
    $('exchange-export').disabled=!ready;
    $('exchange-relief').disabled=!ready || Boolean(lesson);
    selector.disabled=Boolean(lesson);
    $('exchange-net').hidden=selector.value!=='surface';
    $('exchange-lesson').hidden=!lesson;
  }
  function load() {
    if(outlineDialog.open)outlineDialog.close();
    ++revision; current=null; lesson=null; valid=true; selected=0; patchIndex=0;
    const saved=window.projectExtras?.cadExchange;
    if(saved && (saved.version!==1 || !validSurface(saved.surface)) ||
        (saved?.reference && (typeof saved.reference.content!=='string' || typeof saved.reference.name!=='string'))) {
      valid=false; status('Unsupported exchange document. Open a supported project to edit surfaces and STEP files.');
    }
    doc=valid && saved ? clone(saved) : {version:1,surface:saddle(),reference:null};
    selector.value=doc.reference?'reference':'surface';
    if(dialog.open) refresh();
  }
  function inputs() {
    const sheet=activeSurface(), surface=activePatch(), poles=surface.poles;
    const count=sheet.patches?.length || 1, locked=!valid || offline || Boolean(lesson);
    const list=$('exchange-patch');
    list.replaceChildren(...Array.from({length:count},(_,i)=>new Option('Patch '+(i+1), i)));
    list.value=String(patchIndex);
    const axis=$('exchange-axis');
    if(sheet.axis)axis.value=sheet.axis;
    axis.disabled=count>1 || locked;
    $('exchange-append').disabled=locked || count>=16;
    $('exchange-remove').disabled=locked || count===1;
    $('exchange-continuity').value=surface.continuity || 'position';
    $('exchange-seam-label').hidden=patchIndex===0;
    for(const id of ['exchange-trim','exchange-untrim','exchange-outline','exchange-continuity'])$(id).disabled=locked;
    trimFields.forEach((key,i)=>{$('exchange-'+key).value=(sheet.trim||fullTrim)[i];});
    const points=poles.flat();
    $('exchange-basis').value=surface.kind||'bezier';
    $('exchange-degrees').hidden=surface.kind!=='bspline';
    $('exchange-du').value=surface.degrees?.[0]??Math.min(3,poles.length-1);
    $('exchange-dv').value=surface.degrees?.[1]??Math.min(3,poles[0].length-1);
    $('exchange-row').disabled=!valid || offline || Boolean(lesson) || poles.length>=8 || (count>1 && sheet.axis==='v');
    $('exchange-column').disabled=!valid || offline || Boolean(lesson) || poles[0].length>=8 || (count>1 && sheet.axis==='u');
    pointList.replaceChildren();
    const columns=activePatch().poles[0].length;
    points.forEach((p,i)=>pointList.add(new Option('Row '+(1+Math.floor(i/columns))+' · column '+(1+i%columns),i)));
    selected=Math.min(selected,points.length-1);
    pointList.value=String(selected);
    $('exchange-weight').value=surface.weights?.[Math.floor(selected/columns)]?.[selected%columns]??1;
    const along=sheet.axis==='u' ? Math.floor(selected/columns) : selected%columns;
    const driven=patchIndex>0 && (along===0 || (along===1 && surface.continuity==='tangent'));
    $('exchange-driven').hidden=!driven;
    for(const field of ['X','Y','Z','weight'])$('exchange-'+field).disabled=driven;
    ['X','Y','Z'].forEach((a,i)=>{$('exchange-'+a).value=points[selected][i];});
  }
  function paint() {
    const ctx=canvas.getContext('2d');
    const w=canvas.width,h=canvas.height;
    ctx.clearRect(0,0,w,h); projectedPoles=[];
    if(!current)return;
    const control=selector.value==='surface'?activePatch().poles.flat():[];
    const low=current.bounds.slice(0,3),high=current.bounds.slice(3);
    for(const p of control)for(let i=0;i<3;i++){low[i]=Math.min(low[i],p[i]);high[i]=Math.max(high[i],p[i]);}
    const center=low.map((x,i)=>(x+high[i])/2);
    const scale=Math.min(w,h)*.89/Math.max(.001,Math.hypot(...high.map((x,i)=>x-low[i])));
    const ca=Math.cos(yaw),sa=Math.sin(yaw),cb=Math.cos(tilt),sb=Math.sin(tilt);
    function project(p) {
      const [x,y,z]=p.map((x,i)=>x-center[i]), d=x*sa+y*ca;
      return [w/2+scale*(x*ca-y*sa),h/2-scale*(d*sb+z*cb),z*sb-d*cb];
    }
    const vertices=current.vertices.map(project);
    const depth=tri=>tri.reduce((sum,i)=>sum+vertices[i][2],0);
    const faces=current.triangles.map(tri=>[tri,depth(tri)]).sort((a,b)=>a[1]-b[1]);
    for(const [tri] of faces){
      const [a,b,c]=tri.map(i=>vertices[i]);
      const front=(b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0]);
      const dx=(b[1]-a[1])*(c[2]-a[2])-(b[2]-a[2])*(c[1]-a[1]);
      const dy=(b[2]-a[2])*(c[0]-a[0])-(b[0]-a[0])*(c[2]-a[2]);
      const shade=Math.abs((.5*dx+.3*dy+.8*front/scale)/(Math.hypot(dx,dy,front/scale)||1));
      ctx.fillStyle='hsl(184 43% '+(33+34*shade)+'%)';ctx.strokeStyle=ctx.fillStyle;
      ctx.beginPath();ctx.moveTo(a[0],a[1]);ctx.lineTo(b[0],b[1]);ctx.lineTo(c[0],c[1]);ctx.closePath();ctx.fill();ctx.stroke();
    }
    if(control.length){
      projectedPoles=control.map(project);
      const cols=activePatch().poles[0].length;
      ctx.strokeStyle='rgba(253,228,153,.78)';
      projectedPoles.forEach(([x,y],i)=>{
        for(const j of [i%cols ? i-1:-1,i-cols]){
          if(j<0)continue;
          ctx.beginPath();ctx.moveTo(x,y);ctx.lineTo(projectedPoles[j][0],projectedPoles[j][1]);ctx.stroke();
        }
      });
      projectedPoles.forEach(([x,y],i)=>{ctx.fillStyle=i===selected?'#ff9e45':'#ffedc3';ctx.beginPath();ctx.arc(x,y,i===selected?6:3.5,0,2*Math.PI);ctx.fill();});
    }
  }
  function payload(){
    if(selector.value==='surface')return {surface:activeSurface()};
    if(selector.value==='reference'){
      if(!doc.reference)throw Error('Choose Import STEP to load an exchange file.');
      return {content:doc.reference.content};
    }
    return {model:CNCSolidExchange.model()};
  }
  async function refresh() {
    const ticket=++revision;
    current=null; inputs(); paint();enableButtons();
    if(!valid){status('Unsupported exchange document. Open a supported project to edit it.');return;}
    if(offline){status('Open the project on a Studio server to preview and exchange CAD geometry.');return;}
    try {
      const data=payload();
      status('Building geometry…');
      const endpoint={surface:'solid-cad/surface',reference:'solid-cad/step/import',part:'solid-cad'}[selector.value];
      const result=await api(endpoint,data);
      if(ticket!==revision)return;
      current=result;
      const size=current.bounds.slice(3).map((x,i)=>(x-current.bounds[i]).toFixed(2)).join(' × ');
      const name=selector.value==='reference' ? doc.reference.name+' · ' : '';
      const detail=current.faces===undefined?'':` · ${current.faces} faces · ${current.solids} solids`;
      status(name+size+' mm'+detail);
    } catch(error) { if(ticket===revision)status(error.message); }
    if(ticket===revision){paint();enableButtons();}
  }
  selector.onchange=refresh;
  $('exchange-basis').onchange=()=>{$('exchange-degrees').hidden=$('exchange-basis').value!=='bspline';};
  pointList.onchange=()=>{selected=Number(pointList.value);inputs();paint();};
  canvas.onpointerdown=e=>{drag=[e.clientX,e.clientY,e.clientX,e.clientY];canvas.setPointerCapture(e.pointerId);};
  canvas.onpointermove=e=>{
    if(!drag)return;
    yaw+=(e.clientX-drag[0])*.012;tilt=Math.max(-1.5,Math.min(1.5,tilt+(e.clientY-drag[1])*.012));
    drag[0]=e.clientX;drag[1]=e.clientY;paint();
  };
  canvas.onpointerup=e=>{
    if(drag && Math.hypot(e.clientX-drag[2],e.clientY-drag[3])<4){
      const rect=canvas.getBoundingClientRect();
      let nearest=15;
      projectedPoles.forEach(([x,y],i)=>{
        const d=Math.hypot((e.clientX-rect.left)-x*rect.width/canvas.width,(e.clientY-rect.top)-y*rect.height/canvas.height);
        if(d<nearest){nearest=d;selected=i;}
      });
      inputs();paint();
    }
    drag=null;
  };
  canvas.onlostpointercapture=()=>{drag=null;};
  canvas.onkeydown=e=>{
    if(!['ArrowUp','ArrowDown','ArrowLeft','ArrowRight'].includes(e.key))return;
    e.preventDefault();
    yaw+=e.key==='ArrowLeft'?-.1:e.key==='ArrowRight'?.1:0;
    tilt=Math.max(-1.5,Math.min(1.5,tilt+(e.key==='ArrowUp'?.1:e.key==='ArrowDown'?-.1:0)));paint();
  };
  function action(id,fn){$(id).onclick=async()=>{try{await fn();}catch(error){status(error.message);}};}
  async function commitSurface(sheet, nextPatch=patchIndex, nextPoint=selected) {
    const ticket=++revision;
    current=null; enableButtons();
    try {
      const result=await api('solid-cad/surface',{surface:sheet});
      if(ticket!==revision)return false;
      if(sheet.kind==='sheet')sheet.patches=result.resolvedPatches;
      if(lesson)lesson.surface=sheet;
      else {doc.surface=sheet;store();}
      patchIndex=nextPatch;selected=nextPoint;
      current=result; inputs();paint();enableButtons();
      const size=result.bounds.slice(3).map((x,i)=>(x-result.bounds[i]).toFixed(2)).join(' × ');
      status(size+' mm · '+result.faces+' faces · 0 solids');
      return true;
    } catch(error) {
      if(ticket===revision){await refresh();if(ticket+1===revision)status(error.message);}
      return false;
    }
  }
  function editablePatch(sheet) {return sheet.patches?.[patchIndex] || sheet;}
  $('exchange-patch').onchange=()=>{
    ++revision;patchIndex=Number($('exchange-patch').value);selected=0;refresh();
  };
  action('exchange-apply',async()=>{
    const sheet=clone(activeSurface()), patch=editablePatch(sheet);
    const point=['X','Y','Z'].map(a=>$('exchange-'+a).valueAsNumber);
    if(!point.every(x=>Number.isFinite(x)&&Math.abs(x)<=100000))throw Error('Enter three finite coordinates within ±100000 mm.');
    const cols=patch.poles[0].length, row=Math.floor(selected/cols), column=selected%cols;
    patch.poles[row][column]=point;
    patch.kind=$('exchange-basis').value;
    if(patch.kind==='bspline')patch.degrees=[$('exchange-du').valueAsNumber,$('exchange-dv').valueAsNumber];
    const weight=$('exchange-weight').valueAsNumber;
    if(!Number.isFinite(weight)||weight<.001||weight>1000)throw Error('Point weight must be from 0.001 to 1000.');
    patch.weights??=patch.poles.map(r=>r.map(()=>1));patch.weights[row][column]=weight;
    const applied=await commitSurface(sheet);
    if(applied && lesson && selected===5 && point[2]>=12){
      lesson.raised=true;
      $('exchange-instruction').textContent='Orbit the patch to inspect its crown. Export STEP to finish the exchange exercise.';
    }
  });
  async function extendNet(axis){
    const sheet=clone(activeSurface()), patch=editablePatch(sheet), poles=patch.poles, weights=patch.weights;
    function extended(a,b){return b.map((x,i)=>2*x-a[i]);}
    if(axis==='u'){
      if(poles.length>=8)return;
      poles.push(poles.at(-1).map((p,i)=>extended(poles.at(-2)[i],p)));
      if(weights)weights.push([...weights.at(-1)]);
    }else{
      if(poles[0].length>=8)return;
      for(const row of poles)row.push(extended(row.at(-2),row.at(-1)));
      if(weights)weights.forEach(row=>row.push(row.at(-1)));
    }
    await commitSurface(sheet);
  }
  action('exchange-row',()=>extendNet('u'));
  action('exchange-column',()=>extendNet('v'));
  action('exchange-append',async()=>{
    const original=clone(activeSurface());
    const axis=original.axis || $('exchange-axis').value;
    const trim=original.trim || fullTrim;
    const sheet=original.kind==='sheet' ? original : {kind:'sheet',axis,trim,patches:[original]};
    if(original.kind!=='sheet'){
      delete original.trim;
      if(original.boundary){sheet.boundary=original.boundary;delete original.boundary;}
    }
    const previous=sheet.patches.at(-1), patch=clone(previous);
    const rows=patch.poles.length, cols=patch.poles[0].length;
    patch.continuity='position';
    patch.weights=patch.poles.map(()=>Array(cols).fill(1));
    for(let i=0;i<rows;i++)for(let j=0;j<cols;j++){
      const edge=axis==='u' ? previous.poles[rows-1][j] : previous.poles[i][cols-1];
      const inside=axis==='u' ? previous.poles[rows-2][j] : previous.poles[i][cols-2];
      const offset=axis==='u'?i:j;
      patch.poles[i][j]=edge.map((value,k)=>value+offset*(value-inside[k]));
      patch.weights[i][j]=(axis==='u'?previous.weights?.[rows-1]?.[j]:previous.weights?.[i]?.[cols-1])??1;
    }
    sheet.patches.push(patch);
    await commitSurface(sheet,sheet.patches.length-1,0);
  });
  action('exchange-remove',async()=>{
    const sheet=clone(activeSurface());
    if(!sheet.patches || !confirm('Remove the last patch from this surface?'))return;
    sheet.patches.pop();
    const next=sheet.patches.length===1 ? {...sheet.patches[0],trim:sheet.trim||fullTrim} : sheet;
    if(sheet.boundary)next.boundary=sheet.boundary;
    await commitSurface(next,Math.min(patchIndex,sheet.patches.length-1),0);
  });
  $('exchange-continuity').onchange=async()=>{
    const sheet=clone(activeSurface());
    editablePatch(sheet).continuity=$('exchange-continuity').value;
    await commitSurface(sheet);
  };
  action('exchange-trim',async()=>{
    const sheet=clone(activeSurface());
    sheet.trim=trimFields.map(key=>$('exchange-'+key).valueAsNumber);
    await commitSurface(sheet);
  });
  action('exchange-untrim',async()=>{
    const sheet=clone(activeSurface());delete sheet.trim;await commitSurface(sheet);
  });
  // Each loop can be drawn by touch or entered numerically with the keyboard.
  const pad=$('outline-pad'), loopSelect=$('outline-loop'), coordinates=$('outline-points');
  let rings=[], ringIndex=0;
  function drawLoops(){
    const ns='http://www.w3.org/2000/svg';
    pad.replaceChildren();
    function draw(tag,attributes){
      const node=document.createElementNS(ns,tag);
      for(const [key,value] of Object.entries(attributes))node.setAttribute(key,value);
      pad.append(node);
    }
    for(let n=10;n<100;n+=10){
      draw('path',{d:`M ${n} 0 V100 M0 ${n} H100`,stroke:'#b6c7c3','stroke-width':.3});
    }
    const sheet=activeSurface();
    for(let i=1;i<(sheet.patches?.length||1);i++){
      const p=100*i/sheet.patches.length;
      draw('path',{d:sheet.axis==='u'?`M${p} 0 V100`:`M0 ${100-p} H100`,stroke:'#52676a','stroke-width':.7,'stroke-dasharray':'2 1'});
    }
    rings.forEach((ring,i)=>{
      const colour=i===ringIndex?'#934312':'#244a51';
      const points=ring.map(([u,v])=>[100*u,100*(1-v)]);
      draw('polygon',{points:points.map(p=>p.join(',')).join(' '),fill:'none',stroke:colour,'stroke-width':.9});
      points.forEach(([x,y])=>draw('circle',{cx:x,cy:y,r:1,fill:colour}));
    });
  }
  function showLoop(){
    loopSelect.replaceChildren(...rings.map((_,i)=>new Option(i?'Hole '+i:'Outer boundary',i)));
    loopSelect.value=String(ringIndex);
    coordinates.value=rings[ringIndex].map(pt=>pt.join(', ')).join('\n');
    $('outline-drop').disabled=ringIndex===0;
    $('outline-hole').disabled=rings.length>=9;
    $('outline-square').disabled=ringIndex!==0;
    $('outline-status').textContent='';
    drawLoops();
  }
  function readCoordinates(){
    const text=coordinates.value.trim();
    const points=text ? text.split(/\n/).map(line=>line.trim().split(/[\s,]+/)) : [];
    if(points.length>64 || points.some(p=>p.length!==2 || p.some(x=>x==='' || !Number.isFinite(Number(x)) || +x<0 || +x>1))){
      $('outline-status').textContent='Enter up to 64 pairs of numbers between 0 and 1.';
      return false;
    }
    rings[ringIndex]=points.map(p=>p.map(Number));
    $('outline-status').textContent='';drawLoops();return true;
  }
  action('exchange-outline',()=>{
    rings=clone(activeSurface().boundary || [[]]);ringIndex=0;showLoop();outlineDialog.showModal();
  });
  coordinates.oninput=readCoordinates;
  loopSelect.onchange=()=>{
    const next=Number(loopSelect.value);
    if(!readCoordinates()){loopSelect.value=String(ringIndex);return;}
    ringIndex=next;showLoop();
  };
  pad.onclick=e=>{
    if(!readCoordinates() || rings[ringIndex].length>=64)return;
    const matrix=pad.getScreenCTM();
    if(!matrix)return;
    const pt=new DOMPoint(e.clientX,e.clientY).matrixTransform(matrix.inverse());
    const unit=x=>Math.max(0,Math.min(1,Math.round(x*10000)/10000));
    rings[ringIndex].push([unit(pt.x/100),unit(1-pt.y/100)]);showLoop();
  };
  $('outline-square').onclick=()=>{rings[0]=[[0,0],[1,0],[1,1],[0,1]];showLoop();};
  $('outline-back').onclick=()=>{rings[ringIndex].pop();showLoop();};
  $('outline-hole').onclick=()=>{if(!readCoordinates())return;rings.push([]);ringIndex=rings.length-1;showLoop();};
  $('outline-drop').onclick=()=>{if(ringIndex>0){rings.splice(ringIndex,1);ringIndex=0;showLoop();}};
  $('outline-cancel').onclick=()=>outlineDialog.close();
  async function commitOutline(clear){
    if(!clear && !readCoordinates())return;
    const sheet=clone(activeSurface());
    if(clear)delete sheet.boundary;
    else sheet.boundary=clone(rings);
    if(await commitSurface(sheet))outlineDialog.close();
    else $('outline-status').textContent=$('exchange-status').textContent;
  }
  $('outline-apply').onclick=()=>commitOutline(false);
  $('outline-clear').onclick=()=>commitOutline(true);
  action('exchange-import',()=>{$('exchange-file').click();});
  $('exchange-file').onchange=async e=>{
    const file=e.target.files[0];e.target.value='';if(!file)return;
    try{
      if(file.size>1500000)throw Error('Choose a STEP file up to 1.5 MB.');
      const ticket=++revision;
      const reader=new FileReader();
      const encoded=await new Promise((yes,no)=>{reader.onload=()=>yes(reader.result.split(',')[1]);reader.onerror=()=>no(Error('Could not read the file.'));reader.readAsDataURL(file);});
      const result=await api('solid-cad/step/import',{content:encoded});
      if(ticket!==revision)return;
      doc.reference={name:file.name,content:encoded};store();selector.value='reference';
      current=result;paint();enableButtons();
      status(file.name+' · '+result.faces+' faces · '+result.solids+' solids');
    }catch(error){status(error.message);}
  };
  async function binary(path,data) {
    const res=await fetch('/api/'+path,{method:'POST',headers:{'Content-Type':'application/json','X-Studio-Token':token},body:JSON.stringify(data)});
    if(!res.ok)throw Error((await res.json().catch(()=>({}))).error||'CAD export failed.');
    return res.blob();
  }
  action('exchange-export',async()=>{
    const ticket=revision;
    const blob=await binary('solid-cad/step/export',payload());
    if(ticket!==revision)return;
    download(selector.value+'-geometry.step',blob,'model/step');
    status('STEP file exported.');
    if(lesson?.raised){
      $('exchange-instruction').textContent='Practice complete. Keep this surface in the project or exit to restore your own.';
      $('exchange-lesson-keep').hidden=false;
    }
  });
  action('exchange-add',async()=>{
    if(!current?.solidOnly || !doc.reference)return;
    const reference=doc.reference;
    await CNCSolidExchange.addStep(reference.content);
    if (doc.reference!==reference) return;
    doc.reference=null;store();selector.value='part';dialog.close();
  });
  action('exchange-relief',async()=>{
    if(!current || lesson)return;
    const [x0,y0,z0,x1,y1,z1]=current.bounds;
    if(x1-x0<=1e-6 || y1-y0<=1e-6 || z1-z0<=1e-6)throw Error('Choose geometry with relief across X, Y and Z.');
    const x=Number($('solid-x').value), y=Number($('solid-y').value);
    if(![x,y].every(Number.isFinite)||x<0||y<0||x+x1-x0>Number($('width').value)+1e-6||y+y1-y0>Number($('height').value)+1e-6)
      throw Error('Adjust the part placement to fit the work area.');
    const ticket=revision, file=await binary('solid-cad/exchange-stl',payload());
    if(ticket!==revision || !current)return;
    const transfer=new DataTransfer();transfer.items.add(new File([file],'geometry.stl',{type:'model/stl'}));
    $('reliefFile').files=transfer.files;
    for(const [key,val] of Object.entries({shapeX:x,shapeY:y,shapeW:x1-x0,shapeH:y1-y0,reliefDepth:z1-z0}))$(key).value=val;
    $('mode').value='router';modeUI();invalidate();dialog.close();
    CNCToolTabs.open('design','shapes');CNCToolTabs.open('machine','relief');message('Geometry loaded for top-access relief. Review depth, cutter and stepover.');
  });
  action('exchange-guide',()=>{
    lesson={surface:saddle(true),raised:false,previousPatch:patchIndex};patchIndex=0;selected=5;
    $('exchange-instruction').textContent='Raise row 2, column 2 to Z = 12 mm and apply the point. The control net pulls the smooth patch toward it.';
    $('exchange-lesson-keep').hidden=true;return refresh();
  });
  action('exchange-lesson-exit',()=>{patchIndex=lesson.previousPatch;lesson=null;return refresh();});
  action('exchange-lesson-keep',()=>{doc.surface=lesson.surface;lesson=null;store();return refresh();});
  dialog.addEventListener('close',()=>{++revision;if(lesson)patchIndex=lesson.previousPatch;lesson=null;});
  open.onclick=()=>{dialog.showModal();refresh();};
  $('exchange-close').onclick=()=>dialog.close();
  window.addEventListener('cnc-project-loaded',load);
  load();
})();
