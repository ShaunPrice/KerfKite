/* Fractional stations match the tab generator's compensated contour lengths. */
((root)=>{
  'use strict';
  function stations(points) {
    if(!Array.isArray(points)||points.length<4)throw Error('Choose a closed contour.');
    const last=points.at(-1),first=points[0];
    if(Math.hypot(last[0]-first[0],last[1]-first[1])>1e-6)throw Error('The cutter path must be closed.');
    const edges=[];let length=0;
    for(let i=1;i<points.length;i++){
      const [x,y]=points[i-1],dx=points[i][0]-x,dy=points[i][1]-y,span=Math.hypot(dx,dy);
      if(span>0){edges.push({x,y,dx,dy,span,at:length});length+=span;}
    }
    if(!length)throw Error('Contour has no length.');
    return {edges,length};
  }
  function projectStation(contour,x,y) {
    let best=Infinity,fraction=0;
    for(const e of contour.edges){
      const t=Math.max(0,Math.min(1,((x-e.x)*e.dx+(y-e.y)*e.dy)/(e.span*e.span)));
      const d=(x-e.x-t*e.dx)**2+(y-e.y-t*e.dy)**2;
      if(d<best){best=d;fraction=(e.at+t*e.span)/contour.length;}
    }
    return {fraction:fraction%1,distance:Math.sqrt(best)};
  }
  function atStation(contour,f) {
    const station=f*contour.length;
    const e=contour.edges.find(e=>e.at+e.span>=station)||contour.edges.at(-1);
    const t=(station-e.at)/e.span;return [e.x+t*e.dx,e.y+t*e.dy];
  }
  if(typeof module!=='undefined'&&module.exports)module.exports={stations,projectStation,atStation};
  if(typeof document==='undefined')return;
  const open=document.createElement('button');open.type='button';open.id='placeTabs';open.textContent='Place tabs on cutter contour';
  const load=document.createElement('button');load.type='button';load.textContent='Load assigned tab settings';
  $('assignHoldingTabs').before(load,open);
  const dialog=document.createElement('dialog');dialog.id='tab-map';
  dialog.style.cssText='width:min(38rem,90vw);max-height:88vh;overflow:auto';
  dialog.innerHTML=`<h2>Place holding tabs</h2><label>Cutter contour<select id="tab-outline"></select></label>
    <svg id="tab-map-svg" viewBox="0 0 100 100" role="img" aria-label="Compensated cutter contour and tab centres" style="width:100%;height:290px;touch-action:none;background:#edf3f3"></svg>
    <p class="hint">Click the contour to add a tab. Drag a marker to move it. Each percentage applies to every contour on this object.</p>
    <label>Selected tab · % from contour start<input id="tab-station" type="number" step=".1" min="0" max="99.99999"></label>
    <p id="tab-map-info" role="status"></p><div class="buttons"><button id="tab-delete">Remove selected tab</button>
    <button id="tab-keep">Use positions</button><button id="tab-cancel">Cancel</button></div>`;
  document.body.append(dialog);
  dialog.setAttribute('aria-label','Place holding tabs');
  let contours=[],positions=[],idx=-1,drag=null,before='',object=null;
  const svg=$('tab-map-svg'),which=()=>contours[Number($('tab-outline').value)||0];
  const current=()=>JSON.stringify([objects[selected]?.paths,$('mode').value,$('operation').value,$('tool_diameter').value,$('width').value,$('height').value]);
  function values(config) {
    if(config.positions)return config.positions.slice();
    const n=config.count;
    if(!Number.isInteger(n)||n<1||n>100)throw Error('Choose 1–100 tabs.');
    return Array.from({length:n},(_,i)=>(i+.5)/n);
  }
  load.onclick=task(()=>{
    const o=selectedObject(),op=o.operationSettings,t=op?.tabs;
    if(!t)throw Error('Choose an object with assigned holding tabs.');
    $('tabHeight').value=t.height;$('tabWidth').value=t.width;
    $('tabStockThickness').value=op.stock_thickness??$('stock_thickness').value;
    $('tabPlacement').value=t.positions?'positions':'count';$('tabPlacement').onchange();
    if(t.positions)$('tabPositions').value=t.positions.map(f=>+(f*100).toFixed(5)).join(', ');
    else $('tabCount').value=t.count;
  });
  function render(){
    const c=which();if(!c)return;
    const size=Math.max(c.box[2]-c.box[0],c.box[3]-c.box[1],1),pad=size*.08;
    svg.setAttribute('viewBox',[c.box[0]-pad,-c.box[3]-pad,c.box[2]-c.box[0]+2*pad,c.box[3]-c.box[1]+2*pad].join(' '));
    const path=svgNode('path',{d:'M'+c.path.map(([x,y])=>x+','+(-y)).join(' L'),fill:'none',stroke:'#285665','stroke-width':2,'vector-effect':'non-scaling-stroke'});
    svg.replaceChildren(path);
    positions.forEach((f,i)=>{
      const [x,y]=atStation(c,f);
      const dot=svgNode('circle',{cx:x,cy:-y,r:Math.max(size*.014,.02),fill:i===idx?'#b74f11':'#227c68','data-index':i});
      dot.style.cursor='grab';svg.append(dot);
    });
    $('tab-station').disabled=idx<0;$('tab-delete').disabled=idx<0;$('tab-keep').disabled=!positions.length;
    $('tab-station').value=idx<0?'':+(positions[idx]*100).toFixed(5);
    $('tab-map-info').textContent=positions.length+' tabs · cutter contour '+c.length.toFixed(2)+' mm';
  }
  open.onclick=task(async()=>{
    object=selectedObject();before=current();
    if($('mode').value!=='router'||!['line','inside','outside'].includes($('operation').value)||object.depths)throw Error('Select a closed router line, inside profile or outside profile.');
    if(offline)throw Error('Open the Studio workstation or hosted editor to position tabs on compensated cutter paths.');
    positions=$('tabPlacement').value==='count'?values({count:$('tabCount').valueAsNumber}):
      $('tabPositions').value.split(',').map(v=>v.trim()?Number(v)/100:NaN);
    if(!positions.length||positions.length>100||positions.some(v=>!Number.isFinite(v)||v<0||v>=1))throw Error('Enter valid starting tab positions between 0% and 100%.');
    const p=Object.fromEntries(fields.map(k=>[k,$(k).type==='number'?Number($(k).value):$(k).value]));
    const job=await api('generate',{...p,paths:object.paths,depth:.1,step:.1,passes:1,tabs:false,router_entry:'plunge'});
    if(object!==objects[selected]||before!==current())throw Error('Selection changed. Open tab placement again.');
    contours=job.prepared_paths.map(path=>({...stations(path),path,box:bounds([path])}));
    if(!contours.length)throw Error('The operation produced no closed contours.');
    $('tab-outline').replaceChildren(...contours.map((c,i)=>new Option('Contour '+(i+1)+' · '+c.length.toFixed(2)+' mm',String(i))));
    idx=positions.length?0:-1;drag=null;dialog.showModal();render();
  });
  function pick(e){
    const p=new DOMPoint(e.clientX,e.clientY).matrixTransform(svg.getScreenCTM().inverse());
    return projectStation(which(),p.x,-p.y);
  }
  svg.onpointerdown=e=>{
    if(e.button!==0)return;
    const i=e.target.dataset.index;
    if(i===undefined){
      const hit=pick(e),ctm=svg.getScreenCTM();
      if(hit.distance>12/Math.hypot(ctm.a,ctm.b)||positions.length>=100)return;
      idx=positions.length;positions.push(hit.fraction);
    }else idx=Number(i);
    e.preventDefault();drag=e.pointerId;svg.setPointerCapture(e.pointerId);render();
  };
  svg.onpointermove=e=>{if(drag===e.pointerId){positions[idx]=pick(e).fraction;render();}};
  svg.onpointerup=svg.onpointercancel=e=>{if(drag===e.pointerId){drag=null;if(svg.hasPointerCapture(e.pointerId))svg.releasePointerCapture(e.pointerId);}};
  $('tab-outline').onchange=render;
  $('tab-station').onchange=()=>{const n=$('tab-station').valueAsNumber;if(Number.isFinite(n)&&n>=0&&n<100&&idx>=0)positions[idx]=n/100;render();};
  $('tab-delete').onclick=()=>{if(idx>=0)positions.splice(idx,1);idx=Math.min(idx,positions.length-1);render();};
  $('tab-cancel').onclick=()=>dialog.close();
  $('tab-keep').onclick=task(()=>{
    if(object!==objects[selected]||current()!==before)throw Error('Selection changed. Reopen tab placement.');
    $('tabPlacement').value='positions';$('tabPlacement').onchange();
    $('tabPositions').value=positions.map(v=>Math.min(99.99999,+(v*100).toFixed(5))).sort((a,b)=>a-b).join(', ');
    dialog.close();message('Positions copied. Assign the tabs to apply these settings to the selected object.');
  });
})(globalThis);
