/* Work-area points, layout groups and optional placement snapping. */
((root) => {
  'use strict';
  function members(items,index) {
    const id=items[index]?.layoutGroup;
    if(typeof id!=='string'||!id) return items[index] ? [index] : [];
    return items.flatMap((o,i)=>o.layoutGroup===id?[i]:[]);
  }
  function extent(items,indices) {
    return indices.map(i=>CNCLayout.extent(items[i])).reduce(
      (out,b)=>[Math.min(out[0],b[0]),Math.min(out[1],b[1]),Math.max(out[2],b[2]),Math.max(out[3],b[3])],
      [Infinity,Infinity,-Infinity,-Infinity]);
  }
  function nearest(point,points,maxDistance) {
    let best=point, distance=maxDistance*maxDistance;
    for(const p of points){
      const dx=point[0]-p[0],dy=point[1]-p[1],d=dx*dx+dy*dy;
      if(d<=distance){best=p;distance=d;}
    }
    return best;
  }
  function candidates(indices) {
    const skip=new Set(indices),visible=new Set(CNCDesignLayers.layers().filter(l=>l.visible!==false).map(l=>l.id));
    const ctm=$('canvas').getScreenCTM();
    const cell=8/Math.max(.001,Math.hypot(ctm.a,ctm.b)),bins=new Map();
    for(const [i,o] of objects.entries()){
      if(skip.has(i)||!visible.has(o.designLayerId||'default'))continue;
      for(const path of o.paths)for(const point of path){
        const key=Math.floor(point[0]/cell)+','+Math.floor(point[1]/cell);
        if(!bins.has(key))bins.set(key,[]);
        bins.get(key).push(point);
      }
    }
    return {bins,cell};
  }
  function snap(point,points,bypass=false) {
    if(bypass)return point;
    if($('placement-vertices').checked){
      if(points?.bins){
        const {cell,bins}=points, ix=Math.floor(point[0]/cell), iy=Math.floor(point[1]/cell);
        const nearby=[];
        for(let x=ix-1;x<=ix+1;x++)for(let y=iy-1;y<=iy+1;y++){
          const bin=bins.get(x+','+y);
          if(bin)for(const p of bin)nearby.push(p);
        }
        const hit=nearest(point,nearby,cell);
        if(hit!==point)return hit;
      }
    }
    if(!$('placement-grid').checked)return point;
    const pitch=$('placement-pitch').valueAsNumber;
    if(!Number.isFinite(pitch)||pitch<.01||pitch>100)return point;
    return point.map(v=>Math.round(v/pitch)*pitch);
  }

  const api={members,extent,nearest,snap,candidates};
  root.KerfPlacement=api;
  if(typeof module!=='undefined' && module.exports) module.exports=api;
  if(typeof document==='undefined')return;
  const groupControls=document.createElement('div');
  groupControls.innerHTML=`<div class="buttons"><button id="placement-group">Group checked objects</button>
    <button id="placement-ungroup">Ungroup selected layout</button></div>
    <div class="pair"><label>Group left X · mm<input id="placement-x" type="number" step="any"></label>
    <label>Group bottom Y · mm<input id="placement-y" type="number" step="any"></label></div>
    <button id="placement-move">Move selected group</button><p id="placement-info" class="hint" role="status"></p>`;
  $('layoutSelection').after(groupControls);
  const panel=document.createElement('details');panel.id='placementSnapping';
  panel.innerHTML=`<summary>Placement snapping</summary>
    <label><input id="placement-grid" type="checkbox"> Snap to grid</label>
    <label>Grid pitch · mm<input id="placement-pitch" type="number" min=".01" max="100" step="any" value="1"></label>
    <label><input id="placement-vertices" type="checkbox"> Snap to nearby artwork vertices</label>
    <p class="hint">Drag artwork by a vertex, resize a handle, or draw a two-point line. Hold Alt to place freely.</p>`;
  $('design-panel-shapes').append(panel);
  function restoreSnap(){
    const saved=window.projectExtras?.placementSnap;
    $('placement-grid').checked=saved?.grid===true;
    $('placement-vertices').checked=saved?.vertices===true;
    $('placement-pitch').value=Number.isFinite(saved?.pitch)&&saved.pitch>=.01&&saved.pitch<=100?saved.pitch:1;
  }
  panel.addEventListener('change',()=>{
    const pitch=$('placement-pitch').valueAsNumber;
    if(!Number.isFinite(pitch)||pitch<.01||pitch>100){restoreSnap();message('Grid pitch must be 0.01–100 mm.',true);return;}
    window.projectExtras.placementSnap={grid:$('placement-grid').checked,vertices:$('placement-vertices').checked,pitch};
    window.dispatchEvent(new Event('cnc-project-changed'));
  });
  window.addEventListener('cnc-project-loaded',restoreSnap);
  restoreSnap();
  function update() {
    const indices=members(objects,selected),isGroup=indices.length>1;
    for(const key of ['ungroup','move','x','y'])$('placement-'+key).disabled=!isGroup;
    $('placement-info').textContent=isGroup ? 'Layout group · '+indices.length+' members. Drag a member to move the group.' : 'Use the layout checklist to group objects.';
    if(isGroup){
      const [x,y]=extent(objects,indices);
      $('placement-x').value=Number(x.toFixed(4));$('placement-y').value=Number(y.toFixed(4));
    }else{ $('placement-x').value='';$('placement-y').value=''; }
    if(isGroup){
      const [x,y,mx,my]=extent(objects,indices);
      if(CNCDesignLayers.layerFor(objects[selected],CNCDesignLayers.layers()).visible)
        $('selection').prepend(svgNode('rect',{x,y:Number($('height').value)-my,width:mx-x,height:my-y,
          fill:'none',stroke:'#176a9a','stroke-dasharray':'4 3','vector-effect':'non-scaling-stroke','pointer-events':'none'}));
    }
  }
  $('placement-group').onclick=task(()=>{
    const checked=Array.from($('layoutSelection').querySelectorAll('input')).flatMap((input,i)=>input.checked?[i]:[]);
    const indices=[...new Set(checked.flatMap(i=>members(objects,i)))];
    if(indices.length<2)throw Error('Check at least two objects to make a layout group.');
    const id=crypto.randomUUID();
    for(const i of indices)objects[i].layoutGroup=id;
    selected=indices[0];invalidate();draw();
  });
  $('placement-ungroup').onclick=task(()=>{
    for(const i of members(objects,selected))delete objects[i].layoutGroup;
    invalidate();draw();
  });
  $('placement-move').onclick=task(()=>{
    const indices=members(objects,selected),[x,y,mx,my]=extent(objects,indices);
    const nx=$('placement-x').valueAsNumber,ny=$('placement-y').valueAsNumber;
    if(![nx,ny].every(Number.isFinite)||nx<0||ny<0||nx+mx-x>$('width').valueAsNumber||ny+my-y>$('height').valueAsNumber)throw Error('Place the entire group inside the work area.');
    objects=CNCLayout.apply(objects,indices.map(index=>({index,dx:nx-x,dy:ny-y})));
    invalidate();draw();
  });
  const previousDraw=draw;
  draw=function(...args){previousDraw(...args);update();};
  update();
})(globalThis);
