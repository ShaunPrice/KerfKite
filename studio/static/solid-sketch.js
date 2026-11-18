/* Sketch edits remain local until Apply validates and commits the complete loop. */
(() => {
  'use strict';
  const namespace='http://www.w3.org/2000/svg';
  const numeric=/^[+-]?(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?$/;
  const driven=value=>typeof value==='string' && !numeric.test(value.trim());
  function graphic(tag,attributes={}) {
    const node=document.createElementNS(namespace,tag);
    for(const [key,value] of Object.entries(attributes))node.setAttribute(key,value);
    return node;
  }
  // A geometric relation follows the original vertices only while its edge still exists.
  function remapRelations(relations, count, indices) {
    const positions=new Map();
    indices.forEach((old,index)=>{if(old!==null)positions.set(old,index);});
    return relations.map((rel,k)=>{
      function vertex(old) {
        if(!positions.has(old))throw Error('Remove or edit constraint '+(k+1)+' before changing its vertex.');
        return positions.get(old);
      }
      function edge(old) {
        const first=vertex(old), last=vertex((old+1)%count);
        if((first+1)%indices.length!==last)throw Error('Remove or edit constraint '+(k+1)+' before splitting its edge.');
        return first;
      }
      if(rel.kind==='fixed')return {...rel,point:vertex(rel.point)};
      if(rel.kind==='distance')return {...rel,points:rel.points.map(vertex)};
      return {...rel,edges:rel.edges.map(edge)};
    });
  }
  window.remapSketchRelations=remapRelations;

  window.editSolidSketch=async({feature,parameters})=>{
    if(offline)throw Error('Open the project on the Studio server to edit the sketch plane.');
    let values=feature.points.map(point=>point.slice());
    let fillets=feature.fillets?.slice() || values.map(()=>0);
    let constraints=feature.constraints?.slice() || [];
    const resolve=(validate=false, relations=constraints)=>api('solid-cad/sketch-points',{points:values,parameters,fillets,constraints:relations,kind:feature.kind,validate});
    let revision=0, timer, trimming=false, edgePick=0;
    let outline=null;
    const initial=await resolve();
    outline=initial.outline;
    let coords=initial.points, active=coords.length-1, dragging=null, pending=false,done=false;
    const dialog=document.createElement('dialog');dialog.className='solid-sketch-dialog';
    dialog.setAttribute('aria-label','Sketch plane editor');
    dialog.innerHTML=`<h2>Sketch plane</h2>
      <p class="sketch-help">Click empty space to insert after the selected vertex. Drag a vertex to move it. A linked coordinate follows its parameter.</p>
      <div class="sketch-tools"><label>Grid step (mm)<input data-step type="number" min=".001" max="100000" step="any" value="1"></label>
      <label><input data-snap type="checkbox" checked> Snap to grid</label>
      <button data-fit>Fit sketch</button><button data-trim aria-expanded="false">Trim crossing</button></div>
      <fieldset data-trim-panel hidden><legend>Join crossing edges</legend>
      <p class="sketch-help">Pick two edges on the drawing or use their numbers. Choose the portion to keep and check the highlighted loop.</p>
      <div class="sketch-tools"><label>First edge<select data-first></select></label><label>Second edge<select data-second></select></label>
      <label>Retained loop<select data-retain><option value="0">Between the two edges</option><option value="1">Around the closing edge</option></select></label>
      <button data-cut>Keep highlighted loop</button><button data-quit>Cancel trim</button></div></fieldset>
      <svg data-plane viewBox="0 0 600 360" aria-label="Editable planar sketch" role="group" tabindex="0"></svg>
      <div class="sketch-tools"><label>Vertex<select data-vertex></select></label><label>Coordinate<select data-axis></select></label>
      <label>Driving parameter<select data-link></select></label><button data-assign>Link axis</button><button data-release>Release axis</button></div>
      <div class="sketch-tools"><button data-horizontal>Make next edge horizontal</button><button data-vertical>Make next edge vertical</button></div>
      <div class="sketch-tools"><label>Corner fillet radius (mm or parameter)<input data-radius maxlength="160"></label>
      <button data-round>Set fillet</button><button data-unround>Remove fillet</button></div>
      <details data-relations><summary>Geometric constraints</summary>
      <p class="sketch-help">Fix a point, dimension a point pair, or relate two edges. Each edge starts at the selected point and ends at the next point.</p>
      <div class="sketch-tools">
      <label>Relation<select data-relation-kind>
      <option value="fixed">Fixed point</option><option value="distance">Point distance</option>
      <option value="parallel">Parallel edges</option><option value="perpendicular">Perpendicular edges</option>
      <option value="equal">Equal edge lengths</option><option value="angle">Directed edge angle</option></select></label>
      <label>First reference<select data-ref-a></select></label><label data-other-ref>Second reference<select data-ref-b></select></label>
      <label data-val-label><span data-dimension-label>Dimension</span><input data-val maxlength="160"></label>
      <label data-fixed-y-label>Fixed Y or Z (mm)<input data-fixed-y maxlength="160"></label>
      </div>
      <div class="sketch-tools"><button data-add-rel>Add constraint</button>
      <label>Existing constraints<select data-rel-list></select></label>
      <button data-change-rel>Replace selected</button><button data-remove-rel>Remove selected</button></div></details>
      <output data-dof aria-live="polite"></output>
      <div class="sketch-tools"><button data-remove>Remove vertex</button><span data-position></span></div>
      <p data-status role="status"></p>
      <div class="sketch-tools"><button data-apply class="primary">Apply sketch</button><button data-cancel>Cancel sketch</button></div>`;
    document.body.append(dialog);dialog.showModal();
    const get=key=>dialog.querySelector('[data-'+key+']');
    const surface=get('plane'), axes=feature.kind==='revolve'?['Radius','Z']:['X','Y'];
    axes.forEach((name,i)=>get('axis').add(new Option(name,i)));
    function refreshParameterChoices() {
      const old=get('link').value;
      const names=Object.keys(parameters);
      get('link').replaceChildren(...names.map(name=>new Option(name,name)));
      if(names.includes(old)) get('link').value=old;
      if(!names.length)get('link').add(new Option('No parameters',''));
    }
    refreshParameterChoices();
    let xmid=0,ymid=0,scale=1;
    function ids(){return values.map((_,i)=>i);}
    function fit(){
      const xs=coords.map(p=>p[0]), ys=coords.map(p=>p[1]);
      const lowX=Math.min(0,...xs),highX=Math.max(0,...xs),lowY=Math.min(0,...ys),highY=Math.max(0,...ys);
      xmid=(lowX+highX)/2;ymid=(lowY+highY)/2;
      scale=Math.min(490/Math.max(12,highX-lowX),260/Math.max(12,highY-lowY));
      draw();
    }
    const screen=([x,y])=>[300+(x-xmid)*scale,180-(y-ymid)*scale];
    function readPointer(e) {
      const point=new DOMPoint(e.clientX,e.clientY).matrixTransform(surface.getScreenCTM().inverse());
      const snap=get('snap').checked,step=get('step').valueAsNumber;
      if(snap && (!Number.isFinite(step)||step<.001||step>100000))throw Error('Choose a grid step from 0.001 to 100000 mm.');
      return [(point.x-300)/scale+xmid,ymid-(point.y-180)/scale].map(v=>Number((snap?Math.round(v/step)*step:v).toFixed(5)));
    }
    function controls() {
      get('vertex').replaceChildren(...coords.map((_,i)=>new Option('Point '+(i+1),i)));
      get('vertex').value=active;
      const axis=Number(get('axis').value);
      const value=values[active]?.[axis];
      get('assign').disabled=pending||active<0||!get('link').value;
      get('release').disabled=pending||!driven(value);
      get('remove').disabled=pending||active<0;
      get('apply').disabled=pending||trimming||coords.length<3;
      get('radius').value=fillets[active] ?? '';
      get('round').disabled=pending||active<0||trimming;
      get('unround').disabled=get('round').disabled||!fillets[active];
      get('horizontal').disabled=get('vertical').disabled=pending||trimming||coords.length<2;
      get('trim').disabled=pending||coords.length<4;
      get('relations').hidden=trimming;
      if(trimming) {
        for(const key of ['assign','release','remove','vertex','axis','link','radius'])get(key).disabled=true;
      } else if(!pending) {
        for(const key of ['vertex','axis','link','radius'])get(key).disabled=false;
      }
      get('position').textContent=active<0?'':axes.map((name,i)=>{
        const input=values[active][i], actual=coords[active][i];
        const solved=driven(input)||Math.abs(Number(input)-actual)>1e-8;
        return name+' '+input+(solved?' = '+Number(actual.toFixed(8)):'');
      }).join('  ·  ');
    }
    function draw(){
      const plot=document.createDocumentFragment();
      const spacing=get('step').valueAsNumber;
      if(Number.isFinite(spacing)&&spacing>0){
        // Coarsen the displayed grid while retaining the selected numeric snap.
        let grid=spacing;
        while(grid*scale<10)grid*=10;
        const px=screen([0,0]),gap=grid*scale;
        for(let x=(px[0]%gap+gap)%gap;x<600;x+=gap)plot.append(graphic('line',{x1:x,x2:x,y1:0,y2:360,class:'sketch-grid'}));
        for(let y=(px[1]%gap+gap)%gap;y<360;y+=gap)plot.append(graphic('line',{x1:0,x2:600,y1:y,y2:y,class:'sketch-grid'}));
      }
      const [zx,zy]=screen([0,0]);plot.append(graphic('line',{x1:zx,x2:zx,y1:0,y2:360,class:'sketch-axis'}),graphic('line',{x1:0,x2:600,y1:zy,y2:zy,class:'sketch-axis'}));
      if(coords.length)plot.append(graphic('polygon',{points:(outline||coords).map(p=>screen(p).join(',')).join(' '),class:'sketch-profile'}));
      if(trimming) drawTrim(plot);
      coords.forEach((pt,i)=>{
        const [cx,cy]=screen(pt);const dot=graphic('circle',{cx,cy,r:i===active?8:6,class:i===active?'sketch-node selected':'sketch-node','data-index':i});
        const caption=graphic('text',{x:cx+9,y:cy-8});caption.textContent=i+1;plot.append(dot,caption);
      });
      surface.replaceChildren(plot);controls();
    }
    function notify(text){get('status').textContent=text;}
    function changed() {
      clearTimeout(timer);
      const version=++revision;
      outline=null;
      draw();
      if(!constraints.length&&!fillets.some(r=>r!==0)){
        updateDiagnostics({diagnostics:{dof:values.flat().filter(v=>!driven(v)).length,redundant:0}});
        return;
      }
      updateDiagnostics(null);
      timer=setTimeout(async()=>{
        try {
          const report=await resolve();
          if(done||version!==revision)return;
          coords=report.points;outline=report.constraint_error?null:report.outline;
          updateDiagnostics(report);
          if(report.constraint_error || report.issue)notify(report.constraint_error||report.issue);
          draw();
        } catch(error) { if(!done&&version===revision)notify(error.message); }
      },100);
    }

    function edgeConstraint(axis) {
      if(trimming||pending||coords.length<2)return;
      const next=(active+1)%coords.length;
      let expr=values[active][axis];
      if(!driven(expr)){
        let n=1, name;
        do { name='edge_'+(axis?'y':'x')+'_'+n++; } while(Object.hasOwn(parameters,name));
        if(Object.keys(parameters).length>=64){notify('The model already has 64 parameters. Link the source coordinate to an existing parameter first.');return;}
        parameters[name]=coords[active][axis];initial.parameters[name]=coords[active][axis];
        values[active][axis]=expr=name;refreshParameterChoices();
      }
      values[next][axis]=expr;
      coords[next][axis]=coords[active][axis];
      changed();
      notify('Points '+(active+1)+' and '+(next+1)+' now share '+axes[axis]+' = '+expr+'.');
    }
    get('horizontal').onclick=()=>edgeConstraint(1);
    get('vertical').onclick=()=>edgeConstraint(0);

    get('round').onclick=async()=>{
      const input=get('radius').value.trim();
      if(!input){notify('Enter a positive radius or a named dimension.');return;}
      const trial=fillets.slice();trial[active]=numeric.test(input)?Number(input):input;
      const version=revision, selected=active;
      try{
        const report=await api('solid-cad/sketch-points',{points:values,parameters,constraints,fillets:trial,kind:feature.kind,validate:true});
        if(done||pending||trimming||version!==revision||selected!==active)return;
        fillets=trial;outline=report.outline;revision++;draw();notify('Corner fillet updated.');
      }catch(error){if(!done)notify(error.message);}
    };
    get('unround').onclick=()=>{fillets[active]=0;changed();notify('Corner fillet removed.');};
    function candidate(i,j) {
      if(i>j)[i,j]=[j,i];
      const n=coords.length;
      if(i<0||j>=n||j-i<2||(i===0&&j===n-1))return null;
      const edges=[i,(i+1)%n,j,(j+1)%n];
      if(edges.some(k=>fillets[k]!==0))return null;
      const hit=window.KerfCornerGeometry.intersection(...edges.map(k=>coords[k]));
      if(!hit||Math.min(hit.t,hit.u,1-hit.t,1-hit.u)<=1e-10)return null;
      const keep=Number(get('retain').value);
      const inside=Array.from({length:j-i},(_,k)=>i+k+1);
      const around=Array.from({length:n-(j-i)},(_,k)=>(j+k+1)%n);
      return {point:hit.point, edges, indices:keep?around:inside};
    }
    function drawTrim(plot) {
      const i=Number(get('first').value),j=Number(get('second').value);
      const item=candidate(i,j);
      get('cut').disabled=!item||pending;
      for(let k=0;k<coords.length;k++){
        const [x1,y1]=screen(coords[k]),[x2,y2]=screen(coords[(k+1)%coords.length]);
        const a={x1,y1,x2,y2};
        if(k===i||k===j)plot.append(graphic('line',{...a,stroke:k===i?'#8d5600':'#405790','stroke-width':3}));
        plot.append(graphic('line',{...a,'data-edge':k,stroke:'transparent','stroke-width':14}));
      }
      if(!item){notify('Choose two nonadjacent straight edges with sharp endpoints and a crossing in both interiors.');return;}
      plot.append(graphic('polygon',{points:[item.point,...item.indices.map(k=>coords[k])].map(p=>screen(p).join(',')).join(' '),
        fill:'#14a47844',stroke:'#13714e','stroke-width':3,'pointer-events':'none','data-retained':''}));
      const [cx,cy]=screen(item.point);
      plot.append(graphic('circle',{cx,cy,r:5,fill:'#fff',stroke:'#13714e','stroke-width':2,'pointer-events':'none'}));
      notify('Retain '+(item.indices.length+1)+' vertices including the join.');
    }
    function closeTrim() {
      trimming=false;get('trim').setAttribute('aria-expanded','false');get('trim-panel').hidden=true;notify('');draw();
    }
    get('trim').onclick=()=>{
      if(trimming){closeTrim();return;}

      const labels=coords.map((_,k)=>'Points '+(k+1)+'–'+((k+1)%coords.length+1));
      for(const name of ['first','second'])get(name).replaceChildren(...labels.map((s,k)=>new Option(s,k)));
      get('first').value=0;get('second').value=2;
      search:for(let i=0;i<coords.length;i++)for(let j=i+2;j<coords.length;j++){
        if(candidate(i,j)){get('first').value=i;get('second').value=j;break search;}
      }
      trimming=true;edgePick=0;get('trim-panel').hidden=false;get('trim').setAttribute('aria-expanded','true');draw();
    };
    for(const key of ['first','second','retain'])get(key).onchange=draw;
    get('quit').onclick=closeTrim;
    get('cut').onclick=()=>{
      const item=candidate(Number(get('first').value),Number(get('second').value));
      if(!item)return;
      const args=item.edges.flatMap(k=>values[k]);
      const used=new Set(constraints.flatMap(r=>r.kind==='fixed'?[r.point]:(r.points || r.edges.flatMap(k=>[k,(k+1)%coords.length]))));
      if(item.edges.some(k=>used.has(k))){notify('Release geometric constraints on the four crossing-edge endpoints before trimming.');return;}
      const point=args.some(driven)?['x','y'].map(axis=>'cross_'+axis+'('+args.join(',')+')'):item.point.slice();
      // Keep the uncut endpoint expressions. A driven join re-evaluates its two original lines.
      if(point.some(v=>typeof v==='string'&&v.length>160)){notify('The join expression is too long. Name the endpoint expressions as model parameters first.');return;}
      try{constraints=remapRelations(constraints,values.length,[null,...item.indices]);}
      catch(error){notify(error.message);return;}
      values=[point,...item.indices.map(k=>values[k])];
      coords=[item.point,...item.indices.map(k=>coords[k])];
      fillets=[0,...item.indices.map(k=>fillets[k])];active=0;outline=null;
      refreshRelations();closeTrim();changed();notify('Trimmed loop ready. Apply sketch to validate and keep it.');
    };
    const relationNames={fixed:'Fix point',distance:'Distance',parallel:'Parallel',perpendicular:'Perpendicular',equal:'Equal lengths',angle:'Angle'};
    function updateDiagnostics(report) {
      const el=get('dof');
      if(!report){el.textContent='Recalculating sketch…';return;}
      if(!report.diagnostics){el.textContent=report.constraint_error || '';return;}
      const {dof,redundant}=report.diagnostics;
      el.textContent=(dof===0?'Fully constrained':dof+' remaining degree'+(dof===1?'':'s')+' of freedom')+
        (redundant?' · '+redundant+' redundant constraint component'+(redundant===1?'':'s'):'');
    }
    function relationLabel(r,i) {
      const refs=r.kind==='fixed'?[r.point]:(r.points||r.edges);
      return (i+1)+'. '+relationNames[r.kind]+' '+refs.map(k=>k+1).join(' / ')+(r.value!==undefined?' = '+r.value:'');
    }
    function formValues() {
      const kind=get('relation-kind').value;
      for(const key of ['ref-a','ref-b']){
        [...get(key).options].forEach((option,i)=>{
          option.textContent=['fixed','distance'].includes(kind)?'Point '+(i+1):'Edge '+(i+1)+' ('+(i+1)+'–'+((i+1)%coords.length+1)+')';
        });
      }
      get('other-ref').hidden=kind==='fixed';
      get('val-label').hidden=!['fixed','distance','angle'].includes(kind);
      get('fixed-y-label').hidden=kind!=='fixed';
      get('dimension-label').textContent=kind==='angle'?'Angle (° or expression)':kind==='fixed'?'Fixed '+axes[0]+' (mm)':'Distance (mm or expression)';
      const a=Number(get('ref-a').value), b=Number(get('ref-b').value);
      if(!coords[a]||!coords[b])return;
      if(kind==='fixed'){get('val').value=coords[a][0];get('fixed-y').value=coords[a][1];}
      else if(kind==='distance')get('val').value=Number(Math.hypot(coords[b][0]-coords[a][0],coords[b][1]-coords[a][1]).toFixed(6));
      else if(kind==='angle'){
        const heading=i=>{const p=coords[i],q=coords[(i+1)%coords.length];return Math.atan2(q[1]-p[1],q[0]-p[0]);};
        get('val').value=Number(((heading(b)-heading(a))*180/Math.PI).toFixed(6));
      }
    }
    function refreshRelations() {
      const options=()=>coords.map((_,i)=>new Option(String(i+1),i));
      get('ref-a').replaceChildren(...options());get('ref-b').replaceChildren(...options());
      if(coords.length>1)get('ref-b').value=1;
      get('rel-list').replaceChildren(...constraints.map((rel,i)=>new Option(relationLabel(rel,i),i)));
      get('change-rel').disabled=get('remove-rel').disabled=!constraints.length;
      if(constraints.length)get('rel-list').selectedIndex=constraints.length-1;
      formValues();
    }
    function loadRelation() {
      const r=constraints[get('rel-list').selectedIndex];if(!r)return;
      get('relation-kind').value=r.kind;
      const refs=r.kind==='fixed'?[r.point]:(r.points||r.edges);
      get('ref-a').value=refs[0];if(refs.length>1)get('ref-b').value=refs[1];
      formValues();
      if(r.kind==='fixed'){get('val').value=r.value[0];get('fixed-y').value=r.value[1];}
      else if(r.value!==undefined)get('val').value=r.value;
    }
    for(const key of ['relation-kind','ref-a','ref-b'])get(key).onchange=formValues;
    get('rel-list').onchange=loadRelation;
    async function applyRelation(action) {
      if(pending||trimming)return;
      const at=get('rel-list').selectedIndex,kind=get('relation-kind').value;
      const a=Number(get('ref-a').value),b=Number(get('ref-b').value);
      const expression=key=>{const text=get(key).value.trim();return numeric.test(text)?Number(text):text;};
      const trial=constraints.slice();
      if(action==='remove')trial.splice(at,1);
      else {
        let rel;
        if(kind==='fixed')rel={kind,point:a,value:[expression('val'),expression('fixed-y')]};
        else if(kind==='distance')rel={kind,points:[a,b],value:expression('val')};
        else rel={kind,edges:[a,b],...(kind==='angle'?{value:expression('val')}:{})};
        if(action==='add')trial.push(rel);else trial[at]=rel;
      }
      const version=revision;
      try {
        const report=await resolve(false,trial);
        if(done||pending||trimming||version!==revision)return;
        if(report.constraint_error && action!=='remove'){notify(report.constraint_error);return;}
        revision++;constraints=trial;coords=report.points;outline=report.outline;
        if(!report.constraint_error){
          // Start subsequent edits at the accepted solution while preserving all named drivers.
          values=values.map((point,i)=>point.map((v,k)=>driven(v)?v:coords[i][k]));
        }
        refreshRelations();if(action!=='remove')get('rel-list').selectedIndex=action==='add'?constraints.length-1:at;
        loadRelation();updateDiagnostics(report);draw();notify(report.constraint_error || report.issue || 'Sketch constraints updated.');
      }catch(error){if(!done)notify(error.message);}
    }
    get('add-rel').onclick=()=>applyRelation('add');
    get('change-rel').onclick=()=>applyRelation('replace');
    get('remove-rel').onclick=()=>applyRelation('remove');
    refreshRelations();updateDiagnostics(initial);if(initial.constraint_error)notify(initial.constraint_error);

    let finish;
    const completion=new Promise(r=>{finish=r;});
    function end(result){if(done)return;done=true;clearTimeout(timer);dialog.close();dialog.remove();finish(result);}
    dialog.oncancel=e=>{e.preventDefault();if(trimming) closeTrim();else end(null);};
    get('cancel').onclick=()=>end(null);
    get('fit').onclick=fit;
    get('step').oninput=draw;
    get('vertex').onchange=()=>{active=Number(get('vertex').value);draw();};
    get('axis').onchange=controls;
    get('remove').onclick=()=>{
      try{constraints=remapRelations(constraints,values.length,ids().filter(k=>k!==active));}
      catch(error){notify(error.message);return;}
      values.splice(active,1);coords.splice(active,1);fillets.splice(active,1);active=Math.min(active,values.length-1);
      refreshRelations();changed();
    };
    get('assign').onclick=()=>{
      const axis=Number(get('axis').value),param=get('link').value;
      if(active<0 || !Object.hasOwn(parameters,param))return;
      values[active][axis]=param;coords[active][axis]=initial.parameters[param];changed();fit();
    };
    get('release').onclick=()=>{const axis=Number(get('axis').value);values[active][axis]=coords[active][axis];changed();};
    surface.onpointerdown=e=>{
      if(e.button!==0||pending)return;
      try {
        const id=e.target.dataset.index;
        if(trimming){
          if(e.target.dataset.edge!==undefined){
            get(edgePick===0?'first':'second').value=e.target.dataset.edge;
            edgePick=1-edgePick;
            draw();
          }
          return;
        }
        if(id!==undefined){
          active=Number(id);
          dragging={id:e.pointerId,before:coords[active].slice(),raw:values[active].slice(),x:e.clientX,y:e.clientY};
          surface.setPointerCapture(e.pointerId);
        }else{
          if(coords.length>=256)throw Error('A sketch supports up to 256 vertices.');
          const next=readPointer(e), order=ids();order.splice(active+1,0,null);
          constraints=remapRelations(constraints,values.length,order);
          active++;values.splice(active,0,next.slice());coords.splice(active,0,next);fillets.splice(active,0,0);refreshRelations();changed();
        }
        notify('');draw();
      }catch(err){notify(err.message);}
    };
    surface.onpointermove=e=>{
      if(!dragging||e.pointerId!==dragging.id || Math.hypot(e.clientX-dragging.x,e.clientY-dragging.y)<2)return;
      try {
        const point=readPointer(e);
        if(constraints.some(r=>r.kind==='fixed'&&r.point===active)){notify('Release the fixed-point constraint to drag this vertex.');return;}
        point.forEach((v,axis)=>{if(!driven(values[active][axis])){coords[active][axis]=v;values[active][axis]=v;}});
        changed();
      }catch(error){notify(error.message);}
    };
    surface.onpointercancel=()=>{if(dragging){values[active]=dragging.raw;coords[active]=dragging.before;changed();}dragging=null;};
    surface.onlostpointercapture=()=>{dragging=null;};
    get('apply').onclick=async()=>{
      if(pending)return;
      pending=true;
      // Closing or cancelling remains available until validation returns.
      const output={points:values.map(p=>p.slice()), fillets:fillets.slice(), parameters:{...parameters}, constraints:constraints.slice()};
      dialog.querySelectorAll('button,input,select').forEach(node=>{if(node!==get('cancel'))node.disabled=true;});
      try{await resolve(true);end(output);}
      catch(err){if(!done){notify(err.message);pending=false;dialog.querySelectorAll('button,input,select').forEach(node=>node.disabled=false);controls();}}
    };
    fit();return completion;
  };
})();
