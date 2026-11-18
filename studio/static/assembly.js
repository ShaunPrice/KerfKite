/* Independent part instances and datum mates. Commit only a solved working copy. */
(() => {
  'use strict';
  const panel = document.getElementById('design-panel-solid');
  const open = document.createElement('button');
  open.id = 'assembly-open';
  open.textContent = 'Assembly mates & interference';
  panel.append(open);
  const dialog = document.createElement('dialog');
  dialog.id = 'assembly-editor';
  dialog.setAttribute('aria-labelledby','assembly-title');
  dialog.style.cssText = 'width:min(880px,94vw);max-height:90vh;overflow:auto';
  dialog.innerHTML = `
    <h2 id="assembly-title">Assembly</h2><div class="buttons"><button id="assembly-example">Try bearing tutorial</button>
    <button id="assembly-add">Insert current solid</button><button id="assembly-update">Replace selected geometry</button><button id="assembly-remove">Remove part</button></div>
    <p id="assembly-lesson" class="hint" hidden>Check the fitted shaft. Change its parent datum X from 20 to 24 mm and solve again to see interference. Restore X to 20 mm to recover clearance.</p>
    <canvas id="assembly-view" width="720" height="300" style="width:100%;height:auto;background:#132a35;touch-action:none" aria-label="Solved assembly. Drag to orbit." tabindex="0"></canvas>
    <p id="assembly-status" role="status"></p>
    <div class="pair"><label>Part<select id="assembly-part"></select></label>
    <label>Mate to<select id="assembly-parent"></select></label></div>
    <div class="pair"><label>Part name<input id="assembly-name" maxlength="80"></label><button id="assembly-rename">Rename part</button></div>
    <div id="assembly-free"><h3>World placement</h3><div class="pair">
    <label>Origin XYZ · mm<input id="assembly-origin"></label><label>Rotation XYZ · °<input id="assembly-rotation"></label></div></div>
    <div id="assembly-mated"><label>Mate type<select id="assembly-kind"><option value="fixed">Fixed frames</option><option value="planar">Coincident planes</option>
    <option value="concentric">Concentric axes</option><option value="point">Coincident points</option></select></label>
    <h3>Local datum frames</h3><div class="pair"><label>Parent origin XYZ · mm<input id="assembly-parent-origin"></label>
    <label>Parent rotation XYZ · °<input id="assembly-parent-rotation"></label>
    <label>Child origin XYZ · mm<input id="assembly-child-origin"></label>
    <label>Child rotation XYZ · °<input id="assembly-child-rotation"></label></div>
    <h3>Joint position</h3><p id="assembly-freedom" class="hint"></p><div class="pair">
    <label>Shift in datum XYZ · mm<input id="assembly-offset"></label><label>Turn in datum XYZ · °<input id="assembly-turn"></label></div></div>
    <details><summary>Additional mates</summary>
    <p class="hint">Add a mate between existing parts. The solver can slide or turn their driving joints. Independent parts stay at their world placements.</p>
    <label>Other part<select id="assembly-target"></select></label>
    <label>Additional mate<select id="assembly-relation"><option value="planar">Coincident planes</option>
    <option value="concentric">Concentric axes</option><option value="point">Coincident points</option><option value="fixed">Fixed frames</option></select></label>
    <div class="pair"><label>Other datum origin XYZ · mm<input id="assembly-extra-parent" value="0, 0, 0"></label>
    <label>Other datum rotation XYZ · °<input id="assembly-extra-parent-turn" value="0, 0, 0"></label>
    <label>Selected datum origin XYZ · mm<input id="assembly-extra-child" value="0, 0, 0"></label>
    <label>Selected datum rotation XYZ · °<input id="assembly-extra-child-turn" value="0, 0, 0"></label></div>
    <button id="assembly-add-relation">Add mate</button><div id="assembly-relations"></div></details>
    <div class="buttons"><button id="assembly-solve">Solve & check</button><button id="assembly-apply">Save assembly to project</button><button id="assembly-close">Close</button></div>`;
  document.body.append(dialog);
  const ui = key => document.getElementById('assembly-' + key);
  const empty = () => ({version:1, parts:[], mates:[]});
  const clone = object => JSON.parse(JSON.stringify(object));
  const zero = [0, 0, 0];
  const choices = {
    fixed:'Both frames coincide. Shift and turn are zero.',
    planar:'Slide in X and Y and turn about Z. Both datum planes share a normal.',
    concentric:'Slide along Z or turn about Z. The two datum Z axes coincide.',
    point:'Turn about X, Y or Z. Both datum origins coincide.'
  };
  let doc, active, solved, epoch = 0, yaw = -.7, pitch = .55;
  const status = msg => { ui('status').textContent = msg; };
  function vector(key) {
    const tokens = ui(key).value.trim().split(/[,\s]+/);
    const values = tokens.map(Number);
    if (tokens.length !== 3 || values.some(v => !Number.isFinite(v))) throw Error('Enter three comma-separated numbers for each XYZ field.');
    return values;
  }
  function setVector(key, values) { ui(key).value = (values || zero).join(', '); }
  function controls() {
    const part = doc.parts.find(p => p.id === active);
    const mate = doc.mates.find(m => m.child === active);
    ui('part').replaceChildren(...doc.parts.map(p => new Option(p.id, p.id)));
    ui('part').value = active || '';
    ui('parent').replaceChildren(new Option('Independent placement',''), ...doc.parts.filter(p => p !== part).map(p => new Option(p.id, p.id)));
    ui('parent').value = mate?.parent || '';
    ui('kind').value = mate?.kind || 'fixed';
    ui('name').value = active || '';
    setVector('origin', part?.origin);
    setVector('rotation', part?.rotation);
    for (const side of ['child', 'parent']) {
      const frame = mate?.[side + 'Frame'];
      setVector(side + '-origin', frame?.origin);
      setVector(side + '-rotation', frame?.rotation);
    }
    setVector('offset', mate?.offset);
    setVector('turn', mate?.rotation);
    for (const key of ['solve', 'remove','parent','part','name','rename']) ui(key).disabled = !part;
    ui('add').disabled = !window.projectExtras?.solidCAD?.features?.length || doc.parts.length >= 24;
    ui('update').disabled = !part || !window.projectExtras?.solidCAD?.features?.length;
    const target = ui('target').value;
    ui('target').replaceChildren(...doc.parts.filter(p=>p.id!==active).map(p=>new Option(p.id,p.id)));
    if (target && target!==active && doc.parts.some(p=>p.id===target)) ui('target').value=target;
    ui('add-relation').disabled=!ui('target').value;
    const host=ui('relations'); host.replaceChildren();
    for (const constraint of doc.constraints || []) {
      const row=document.createElement('p'), remove=document.createElement('button');
      row.append(document.createTextNode(constraint.child+' ↔ '+constraint.parent+' · '+constraint.kind+' '));
      remove.textContent='Remove mate';remove.onclick=run(()=>{
        readForm(); doc.constraints=doc.constraints.filter(c=>c!==constraint); invalidate(); controls();
      });
      row.append(remove);host.append(row);
    }
    visibility();
  }
  function visibility() {
    ui('free').hidden = !active || !!ui('parent').value;
    ui('mated').hidden = !active || !ui('parent').value;
    ui('freedom').textContent = choices[ui('kind').value];
  }
  function readForm() {
    if (!active) return;
    const parent = ui('parent').value;
    let mate;
    if (parent) {
      const frame = side => ({origin: vector(side + '-origin'), rotation: vector(side + '-rotation')});
      mate = {parent, child:active, kind:ui('kind').value, parentFrame:frame('parent'), childFrame:frame('child'), offset:vector('offset'), rotation:vector('turn')};
    } else {
      const origin = vector('origin'), rotation = vector('rotation');
      Object.assign(doc.parts.find(p => p.id === active), {origin, rotation});
    }
    doc.mates = doc.mates.filter(m => m.child !== active);
    if (mate) doc.mates.push(mate);
  }
  function draw() {
    const canvas = ui('view'), ctx = canvas.getContext('2d');
    ctx.clearRect(0, 0, canvas.width, canvas.height);
    if (!solved) return;
    const plot = [], colors = ['#5795af','#dfab64','#82b59c','#a493bf'];
    const touching = new Set(solved.interferences.flatMap(pair => pair.parts));
    let minX=Infinity,minY=Infinity,maxX=-Infinity,maxY=-Infinity;
    Object.entries(solved.parts).forEach(([id, part], index) => {
      const projected = part.vertices.map(([x,y,z]) => {
        const u = Math.cos(yaw)*x - Math.sin(yaw)*y, d = Math.sin(yaw)*x + Math.cos(yaw)*y;
        const v = Math.sin(pitch)*d - Math.cos(pitch)*z;
        minX=Math.min(minX,u);maxX=Math.max(maxX,u);minY=Math.min(minY,v);maxY=Math.max(maxY,v);
        return [u,v,Math.cos(pitch)*d + Math.sin(pitch)*z];
      });
      const color = touching.has(id) ? '#ec7259' : colors[index%colors.length];
      for (const [a,b,c] of part.triangles) {
        const p=projected[a],q=projected[b],r=projected[c];
        const facing=(q[0]-p[0])*(r[1]-p[1])-(q[1]-p[1])*(r[0]-p[0]);
        if(facing<=0)continue;
        const len=Math.hypot((q[1]-p[1])*(r[2]-p[2])-(q[2]-p[2])*(r[1]-p[1]),(q[2]-p[2])*(r[0]-p[0])-(q[0]-p[0])*(r[2]-p[2]),facing);
        plot.push({p,q,r,depth:p[2]+q[2]+r[2],color,shade:.48+.52*facing/len});
      }
    });
    const scale = Math.min(650/Math.max(.001,maxX-minX),255/Math.max(.001,maxY-minY));
    const xy = v => [360+(v[0]-(minX+maxX)/2)*scale,150+(v[1]-(minY+maxY)/2)*scale];
    plot.sort((a,b)=>a.depth-b.depth);
    for(const {p,q,r,color,shade} of plot){
      ctx.fillStyle = 'rgb('+[1,3,5].map(i=>Math.round(parseInt(color.slice(i,i+2),16)*shade)).join(',')+')';
      ctx.beginPath();ctx.moveTo(...xy(p));ctx.lineTo(...xy(q));ctx.lineTo(...xy(r));ctx.closePath();ctx.fill();
    }
  }
  function invalidate() {
    ++epoch; solved=null; draw();
    status('Solve the assembly to check the current placements.');
  }
  async function solveNow() {
    readForm();
    const stamp = ++epoch;
    solved=null;draw();status('Solving mates and checking interference…');
    try {
      const result = await api('assembly/solve', {assembly:doc, mesh:true});
      if (stamp !== epoch) return false;
      solved=result;draw();
      const overlaps=result.interferences.map(p=>p.parts.join(' × ')+' ('+p.volume.toLocaleString(undefined,{maximumSignificantDigits:6})+' mm³)');
      const contacts=result.contacts.map(p=>p.parts.join(' / '));
      status([overlaps.length?'Interference: '+overlaps.join(', '):'No interference detected.', ...contacts.map(p=>'Contact: '+p)].join(' '));
      return true;
    } catch (err) { if (stamp===epoch) throw err; return false; }
  }
  const run = fn => async () => { try { await fn(); } catch(err) { status(err.message); } };
  open.onclick = run(async () => {
    const saved = window.projectExtras?.assembly;
    if (saved && (saved.version!==1 || !Array.isArray(saved.parts) || !Array.isArray(saved.mates) || (saved.constraints!==undefined && !Array.isArray(saved.constraints)))) {message('Unsupported assembly document.');return;}
    doc=clone(saved || empty());active=doc.parts[0]?.id;solved=null;ui('lesson').hidden=true;
    controls();dialog.showModal();draw();status('Insert a solid part or try the bearing tutorial.');
    if (active) await solveNow();
  });
  ui('part').onchange = run(() => {
    const previous=active, next=ui('part').value;
    try {readForm();} catch(err) {ui('part').value=previous;throw err;}
    active=next;controls();
  });
  ui('add-relation').onclick=run(async ()=>{
    readForm();
    const frame=side=>({origin:vector('extra-'+side),rotation:vector('extra-'+side+'-turn')});
    const constraint={child:active,parent:ui('target').value,kind:ui('relation').value,childFrame:frame('child'),parentFrame:frame('parent')};
    doc.constraints??=[];
    if(doc.constraints.length>=48)throw Error('Use up to 48 additional mates.');
    doc.constraints.push(constraint); invalidate(); controls(); await solveNow();
  });
  ui('parent').onchange=visibility;
  ui('kind').onchange=()=>{setVector('offset');setVector('turn');visibility();};
  dialog.addEventListener('input', e=>{if(e.target!==ui('part'))invalidate();});
  ui('add').onclick = run(async () => {
    readForm();let n=1;
    while(doc.parts.some(p=>p.id==='Part '+n))++n;
    active='Part '+n;
    doc.parts.push({id:active,model:clone(window.projectExtras.solidCAD)});
    controls();await solveNow();
  });
  ui('rename').onclick=run(()=>{
    readForm();
    const name=ui('name').value.trim();
    if(!name || doc.parts.some(p=>p.id===name && p.id!==active))throw Error('Choose a unique, nonempty part name.');
    doc.parts.find(p=>p.id===active).id=name;
    for(const m of [...doc.mates,...(doc.constraints||[])]){if(m.child===active)m.child=name;if(m.parent===active)m.parent=name;}
    active=name;invalidate();controls();
  });
  ui('update').onclick=run(async ()=>{
    readForm();
    doc.parts.find(p=>p.id===active).model=clone(window.projectExtras.solidCAD);
    await solveNow();
  });
  ui('remove').onclick = run(async () => {
    if((doc.constraints||[]).some(c=>[c.parent,c.child].includes(active)))throw Error('Remove additional mates referencing this part before removing it.');
    if(doc.mates.some(m=>m.parent===active))throw Error('Release this part’s child mates before removing it.');
    doc.parts=doc.parts.filter(p=>p.id!==active);doc.mates=doc.mates.filter(m=>m.child!==active);
    active=doc.parts[0]?.id;invalidate();controls();if(active)await solveNow();else status('Assembly is empty.');
  });
  ui('example').onclick=run(async ()=>{
    const stamp=++epoch;
    const demo=await api('assembly/example');
    if(epoch!==stamp)return;
    doc=demo;active='Shaft';controls();ui('lesson').hidden=false;await solveNow();
  });
  ui('solve').onclick=run(solveNow);
  ui('apply').onclick=run(async ()=>{
    if(active && !await solveNow())return;
    if(active)window.projectExtras.assembly=clone(doc);else delete window.projectExtras.assembly;
    window.dispatchEvent(new CustomEvent('cnc-project-changed'));dialog.close();
  });
  ui('close').onclick=()=>dialog.close();
  dialog.addEventListener('close',()=>++epoch);
  window.addEventListener('cnc-project-loaded',()=>{if(dialog.open)dialog.close();});
  let drag;
  ui('view').onpointerdown=e=>{drag=[e.clientX,e.clientY];ui('view').setPointerCapture(e.pointerId);};
  ui('view').onpointermove=e=>{
    if(!drag)return;
    yaw+=(e.clientX-drag[0])*.012;pitch=Math.max(-1.56,Math.min(1.56,pitch+(e.clientY-drag[1])*.012));
    drag=[e.clientX,e.clientY];draw();
  };
  ui('view').onlostpointercapture=()=>{drag=null;};
  ui('view').onkeydown=e=>{
    if(!['ArrowLeft','ArrowRight','ArrowUp','ArrowDown'].includes(e.key))return;
    e.preventDefault();
    if(e.key==='ArrowLeft')yaw-=.1;if(e.key==='ArrowRight')yaw+=.1;
    if(e.key==='ArrowUp')pitch=Math.min(1.56,pitch+.1);if(e.key==='ArrowDown')pitch=Math.max(-1.56,pitch-.1);
    draw();
  };
})();
