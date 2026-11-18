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
  window.editSolidSketch=async({feature,parameters})=>{
    if(offline)throw Error('Open the project on the Studio server to edit the sketch plane.');
    let values=feature.points.map(point=>point.slice());
    const resolve=(validate=false)=>api('solid-cad/sketch-points',{points:values,parameters,kind:feature.kind,validate});
    const initial=await resolve();
    let coords=initial.points, active=coords.length-1, dragging=null, pending=false,done=false;
    const dialog=document.createElement('dialog');dialog.className='solid-sketch-dialog';
    dialog.setAttribute('aria-label','Sketch plane editor');
    dialog.innerHTML=`<h2>Sketch plane</h2>
      <p class="sketch-help">Click empty space to insert after the selected vertex. Drag a vertex to move it. A linked coordinate follows its parameter.</p>
      <div class="sketch-tools"><label>Grid step (mm)<input data-step type="number" min=".001" max="100000" step="any" value="1"></label>
      <label><input data-snap type="checkbox" checked> Snap to grid</label>
      <button data-fit>Fit sketch</button></div>
      <svg data-plane viewBox="0 0 600 360" aria-label="Editable planar sketch" role="group" tabindex="0"></svg>
      <div class="sketch-tools"><label>Vertex<select data-vertex></select></label><label>Coordinate<select data-axis></select></label>
      <label>Driving parameter<select data-link></select></label><button data-assign>Link axis</button><button data-release>Release axis</button></div>
      <div class="sketch-tools"><button data-remove>Remove vertex</button><span data-position></span></div>
      <p data-status role="status"></p>
      <div class="sketch-tools"><button data-apply class="primary">Apply sketch</button><button data-cancel>Cancel sketch</button></div>`;
    document.body.append(dialog);dialog.showModal();
    const get=key=>dialog.querySelector('[data-'+key+']');
    const surface=get('plane'), axes=feature.kind==='revolve'?['Radius','Z']:['X','Y'];
    axes.forEach((name,i)=>get('axis').add(new Option(name,i)));
    Object.keys(parameters).forEach(name=>get('link').add(new Option(name,name)));
    if(!Object.keys(parameters).length)get('link').add(new Option('No parameters',''));
    let xmid=0,ymid=0,scale=1;
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
      get('apply').disabled=pending||coords.length<3;
      get('position').textContent=active<0?'':axes.map((name,i)=>name+' '+values[active][i]+(driven(values[active][i])?' = '+coords[active][i]:'')).join('  ·  ');
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
      if(coords.length)plot.append(graphic('polygon',{points:coords.map(p=>screen(p).join(',')).join(' '),class:'sketch-profile'}));
      coords.forEach((pt,i)=>{
        const [cx,cy]=screen(pt);const dot=graphic('circle',{cx,cy,r:i===active?8:6,class:i===active?'sketch-node selected':'sketch-node','data-index':i});
        const caption=graphic('text',{x:cx+9,y:cy-8});caption.textContent=i+1;plot.append(dot,caption);
      });
      surface.replaceChildren(plot);controls();
    }
    function notify(text){get('status').textContent=text;}
    let finish;
    const completion=new Promise(r=>{finish=r;});
    function end(points){if(done)return;done=true;dialog.close();dialog.remove();finish(points);}
    dialog.oncancel=e=>{e.preventDefault();end(null);};
    get('cancel').onclick=()=>end(null);
    get('fit').onclick=fit;
    get('step').oninput=draw;
    get('vertex').onchange=()=>{active=Number(get('vertex').value);draw();};
    get('axis').onchange=controls;
    get('remove').onclick=()=>{values.splice(active,1);coords.splice(active,1);active=Math.min(active,values.length-1);draw();};
    get('assign').onclick=()=>{
      const axis=Number(get('axis').value),param=get('link').value;
      if(active<0 || !Object.hasOwn(initial.parameters,param))return;
      values[active][axis]=param;coords[active][axis]=initial.parameters[param];fit();
    };
    get('release').onclick=()=>{const axis=Number(get('axis').value);values[active][axis]=coords[active][axis];draw();};
    surface.onpointerdown=e=>{
      if(e.button!==0||pending)return;
      try {
        const id=e.target.dataset.index;
        if(id!==undefined){
          active=Number(id);
          dragging={id:e.pointerId,before:coords[active].slice(),raw:values[active].slice(),x:e.clientX,y:e.clientY};
          surface.setPointerCapture(e.pointerId);
        }else{
          if(coords.length>=256)throw Error('A sketch supports up to 256 vertices.');
          const next=readPointer(e);active++;values.splice(active,0,next.slice());coords.splice(active,0,next);
        }
        notify('');draw();
      }catch(err){notify(err.message);}
    };
    surface.onpointermove=e=>{
      if(!dragging||e.pointerId!==dragging.id || Math.hypot(e.clientX-dragging.x,e.clientY-dragging.y)<2)return;
      try {
        const point=readPointer(e);
        point.forEach((v,axis)=>{if(!driven(values[active][axis])){coords[active][axis]=v;values[active][axis]=v;}});
        draw();
      }catch(error){notify(error.message);}
    };
    surface.onpointercancel=()=>{if(dragging){values[active]=dragging.raw;coords[active]=dragging.before;draw();}dragging=null;};
    surface.onlostpointercapture=()=>{dragging=null;};
    get('apply').onclick=async()=>{
      if(pending)return;
      pending=true;
      // Closing or cancelling remains available until validation returns.
      const output=values.map(p=>p.slice());
      dialog.querySelectorAll('button,input,select').forEach(node=>{if(node!==get('cancel'))node.disabled=true;});
      try{await resolve(true);end(output);}
      catch(err){if(!done){notify(err.message);pending=false;dialog.querySelectorAll('button,input,select').forEach(node=>node.disabled=false);controls();}}
    };
    fit();return completion;
  };
})();
