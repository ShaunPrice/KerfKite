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
    <label>Geometry<select id="exchange-kind"><option value="surface">Editable surface patch</option><option value="reference">Imported STEP</option><option value="part">Current solid part</option></select></label>
    <canvas id="exchange-canvas" width="700" height="300" tabindex="0" aria-label="Geometry preview. Drag or use the arrow keys to orbit. Click a control point to edit." style="width:100%;height:260px;background:#193341;border-radius:6px;touch-action:none"></canvas>
    <div id="exchange-net">
      <label>Surface basis<select id="exchange-basis"><option value="bezier">Bézier</option><option value="bspline">Clamped B-spline / NURBS</option></select></label>
      <div class="solid-pair" id="exchange-degrees"><label>U degree<input id="exchange-du" type="number" min="1" max="7"></label><label>V degree<input id="exchange-dv" type="number" min="1" max="7"></label></div>
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

  function saddle(flat = false) {
    return {poles:Array.from({length:4}, (_,i)=>Array.from({length:4}, (_,j)=>
      [20*i, 12*j, flat ? 0 : 3*(i-1.5)*(j-1.5)]))};
  }
  const clone = obj=>JSON.parse(JSON.stringify(obj));
  const selector = $('exchange-kind'), pointList = $('exchange-point'), canvas = $('exchange-canvas');
  let doc, current, revision=0, selected=0, lesson=null, valid=true;
  let yaw=-.63, tilt=.62, projectedPoles=[], drag=null;
  const status = text=>{ $('exchange-status').textContent=text; };
  const activeSurface = ()=>lesson ? lesson.surface : doc.surface;
  function store() {
    window.projectExtras.cadExchange=clone(doc);
    window.dispatchEvent(new Event('cnc-project-changed'));
  }
  function enableButtons() {
    const ready=Boolean(current);
    $('exchange-apply').disabled=!valid || offline;
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
    ++revision; current=null; lesson=null; valid=true; selected=0;
    const saved=window.projectExtras?.cadExchange;
    if(saved && (saved.version!==1 || !Array.isArray(saved.surface?.poles) ||
        saved.surface.poles.length<2 || saved.surface.poles.length>8 || !saved.surface.poles.every(row=>
        Array.isArray(row)&&row.length>=2&&row.length<=8&&row.length===saved.surface.poles[0].length&&
        row.every(p=>Array.isArray(p)&&p.length===3&&p.every(v=>Number.isFinite(v)&&Math.abs(v)<=100000)))) ||
        (saved?.reference && (typeof saved.reference.content!=='string' || typeof saved.reference.name!=='string'))) {
      valid=false; status('Unsupported exchange document. Open a supported project to edit surfaces and STEP files.');
    }
    doc=valid && saved ? clone(saved) : {version:1,surface:saddle(),reference:null};
    selector.value=doc.reference?'reference':'surface';
    if(dialog.open) refresh();
  }
  function inputs() {
    const surface=activeSurface(), poles=surface.poles;
    const points=poles.flat();
    $('exchange-basis').value=surface.kind||'bezier';
    $('exchange-degrees').hidden=surface.kind!=='bspline';
    $('exchange-du').value=surface.degrees?.[0]??Math.min(3,poles.length-1);
    $('exchange-dv').value=surface.degrees?.[1]??Math.min(3,poles[0].length-1);
    $('exchange-row').disabled=!valid || offline || Boolean(lesson) || poles.length>=8;
    $('exchange-column').disabled=!valid || offline || Boolean(lesson) || poles[0].length>=8;
    pointList.replaceChildren();
    const columns=activeSurface().poles[0].length;
    points.forEach((p,i)=>pointList.add(new Option('Row '+(1+Math.floor(i/columns))+' · column '+(1+i%columns),i)));
    selected=Math.min(selected,points.length-1);
    pointList.value=String(selected);
    $('exchange-weight').value=surface.weights?.[Math.floor(selected/columns)]?.[selected%columns]??1;
    ['X','Y','Z'].forEach((a,i)=>{$('exchange-'+a).value=points[selected][i];});
  }
  function paint() {
    const ctx=canvas.getContext('2d');
    const w=canvas.width,h=canvas.height;
    ctx.clearRect(0,0,w,h); projectedPoles=[];
    if(!current)return;
    const control=selector.value==='surface'?activeSurface().poles.flat():[];
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
      const cols=activeSurface().poles[0].length;
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
  action('exchange-apply',async()=>{
    const point=['X','Y','Z'].map(a=>$('exchange-'+a).valueAsNumber);
    if(!point.every(x=>Number.isFinite(x)&&Math.abs(x)<=100000))throw Error('Enter three finite coordinates within ±100000 mm.');
    const surface=clone(activeSurface());
    const cols=surface.poles[0].length;
    const row=Math.floor(selected/cols), column=selected%cols;
    surface.poles[row][column]=point;
    surface.kind=$('exchange-basis').value;
    if(surface.kind==='bspline') surface.degrees=[$('exchange-du').valueAsNumber,$('exchange-dv').valueAsNumber];
    const weight=$('exchange-weight').valueAsNumber;
    if(!Number.isFinite(weight) || weight<.001 || weight>1000)throw Error('Point weight must be from 0.001 to 1000.');
    surface.weights??=surface.poles.map(r=>r.map(()=>1));surface.weights[row][column]=weight;
    const stamp=++revision;
    await api('solid-cad/surface',{surface}); // Preserve the last usable net when validation fails.
    if(stamp!==revision)return;
    if(lesson){
      lesson.surface=surface;
      if(selected===5 && point[2]>=12) {
        lesson.raised=true;
        $('exchange-instruction').textContent='Orbit the patch to inspect its crown. Export STEP to finish the exchange exercise.';
      }
    }else{doc.surface=surface;store();}
    await refresh();
  });
  async function extendNet(axis){
    const surface=clone(activeSurface()), poles=surface.poles, weights=surface.weights;
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
    const stamp=++revision;
    await api('solid-cad/surface',{surface});
    if(stamp!==revision)return;
    doc.surface=surface;store();await refresh();
  }
  action('exchange-row',()=>extendNet('u'));
  action('exchange-column',()=>extendNet('v'));
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
    lesson={surface:saddle(true),raised:false};selected=5;
    $('exchange-instruction').textContent='Raise row 2, column 2 to Z = 12 mm and apply the point. The control net pulls the smooth patch toward it.';
    $('exchange-lesson-keep').hidden=true;return refresh();
  });
  action('exchange-lesson-exit',()=>{lesson=null;return refresh();});
  action('exchange-lesson-keep',()=>{doc.surface=lesson.surface;lesson=null;store();return refresh();});
  dialog.addEventListener('close',()=>{++revision;lesson=null;});
  open.onclick=()=>{dialog.showModal();refresh();};
  $('exchange-close').onclick=()=>dialog.close();
  window.addEventListener('cnc-project-loaded',load);
  load();
})();
