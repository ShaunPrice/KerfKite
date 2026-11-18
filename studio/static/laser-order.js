/* CAM-only contour sequencing. Artwork and editable layer order remain intact. */
((root)=>{
  'use strict';
  const same=(a,b)=>a[0]===b[0]&&a[1]===b[1];
  const closed=p=>p.length>=4 && same(p[0],p.at(-1));
  const distance=(a,b)=>Math.hypot(a[0]-b[0],a[1]-b[1]);
  const valid=p=>Array.isArray(p)&&p.length>=2&&p.every(v=>Array.isArray(v)&&v.length===2&&v.every(Number.isFinite));
  const directed=(a,b,c)=>(b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0]);
  function intersect(a,b,c,d){
    if(Math.max(a[0],b[0])<Math.min(c[0],d[0])||Math.max(c[0],d[0])<Math.min(a[0],b[0])||
       Math.max(a[1],b[1])<Math.min(c[1],d[1])||Math.max(c[1],d[1])<Math.min(a[1],b[1]))return false;
    return directed(a,b,c)*directed(a,b,d)<=0 && directed(c,d,a)*directed(c,d,b)<=0;
  }
  function ringContains(ring,p){
    let inside=false;
    for(let i=1;i<ring.length;i++){
      const a=ring[i-1],b=ring[i];
      if((a[1]>p[1])!==(b[1]>p[1])&&p[0]<(b[0]-a[0])*(p[1]-a[1])/(b[1]-a[1])+a[0])inside=!inside;
    }
    return inside;
  }
  function bounds(path){
    const b=[Infinity,Infinity,-Infinity,-Infinity];for(const [x,y] of path){b[0]=Math.min(b[0],x);b[1]=Math.min(b[1],y);b[2]=Math.max(b[2],x);b[3]=Math.max(b[3],y);}return b;
  }
  function normalized(seq){
    // Lexicographically least cyclic shift, in linear time.
    const n=seq.length;let a=0,b=1,k=0;
    while(a<n && b<n && k<n){
      const x=seq[(a+k)%n],y=seq[(b+k)%n];
      if(x===y){k++;continue;}
      if(x>y){a+=k+1;if(a===b)++a;}else{b+=k+1;if(a===b)++b;}k=0;
    }
    const j=Math.min(a,b);return seq.slice(j).concat(seq.slice(0,j)).join(';');
  }
  function canonical(path){
    const loop=closed(path),p=(loop?path.slice(0,-1):path).map(pt=>pt.join(','));
    const key=seq=>loop?normalized(seq):seq.join(';'),forward=key(p),back=key(p.reverse());
    return (loop?'C':'L')+(forward<back?forward:back);
  }
  function travel(paths,start){
    let length=0,cursor=start;for(const path of paths){length+=distance(cursor,path[0]);cursor=path.at(-1);}return length;
  }
  function sequence(paths,start,opts){
    let removed=0;const seen=new Set(),items=[];
    for(const p of paths){
      if(opts.dedupe){const key=canonical(p);if(seen.has(key)){removed++;continue;}seen.add(key);}
      items.push({p,b:bounds(p),dependencies:new Set()});
    }
    let comparisons=0;
    if(opts.inside)for(const outer of items){
      if(!closed(outer.p))continue;
      for(const inner of items){
        if(outer===inner||inner.b[0]<outer.b[0]||inner.b[1]<outer.b[1]||inner.b[2]>outer.b[2]||inner.b[3]>outer.b[3]||!ringContains(outer.p,inner.p[0]))continue;
        let touches=false;
        for(let i=1;i<outer.p.length&&!touches;i++)for(let j=1;j<inner.p.length;j++){
          if(++comparisons>500000)return null;
          if(intersect(outer.p[i-1],outer.p[i],inner.p[j-1],inner.p[j])){touches=true;break;}
        }
        if(!touches)outer.dependencies.add(inner);
      }
    }
    const ordered=[],done=new Set();let current=start;
    while(ordered.length<items.length){
      let best=null,d=Infinity;
      for(const item of items){
        if(done.has(item)||[...item.dependencies].some(v=>!done.has(v)))continue;
        const gap=distance(current,item.p[0]);
        if(best===null||opts.nearest&&gap<d){best=item;d=gap;}
        if(!opts.nearest)break;
      }
      if(!best)return null;
      ordered.push(best.p);done.add(best);current=best.p.at(-1);
    }
    const original=items.map(o=>o.p), indices=new Map(items.map((v,i)=>[v,i]));
    const originalValid=items.every((v,i)=>[...v.dependencies].every(dep=>indices.get(dep)<i));
    // Greedy ordering can lengthen travel; keep the better valid alternative.
    if(originalValid && travel(original,start)<=travel(ordered,start))return {paths:original,removed};
    return {paths:ordered,removed};
  }
  function eligible(op){
    return op.mode==='laser' && (op.operation||'line')==='line' && (op.passes??1)===1 &&
      ['lead_in','lead_out','ramp_in','ramp_out'].every(k=>!op[k]) && !op.path_powers && !op.path_depths &&
      (!op.laser_pattern || op.laser_pattern==='continuous') && !op.tabs && Array.isArray(op.paths) && op.paths.length && op.paths.every(valid);
  }
  const signature=op=>JSON.stringify(Object.keys(op).filter(k=>k!=='paths').sort().map(k=>[k,op[k]]));
  function plan(job,layers,opts) {
    const original=job.operations, report={before:0,after:0,removed:0,skipped:0,oversize:0};
    if(job.mode!=='laser'||!Array.isArray(original)||layers.length!==original.length)return {job,report};
    const ops=[];let i=0,head=[0,0],oldHead=[0,0];
    while(i<original.length){
      const first=original[i];
      let end=i+1;
      if(!eligible(first)){
        ops.push(first);report.skipped++;
        if(Array.isArray(first.paths)&&first.paths.length&&first.paths.every(valid)){
          report.before+=travel(first.paths,oldHead);report.after+=travel(first.paths,head);oldHead=head=first.paths.at(-1).at(-1);
        }
        i=end;continue;
      }
      const groupKey=signature(first);
      while(end<original.length&&layers[end]===layers[i]&&eligible(original[end])&&signature(original[end])===groupKey)end++;
      const group=original.slice(i,end),paths=group.flatMap(o=>o.paths);
      const next=paths.length<=200 && paths.reduce((n,p)=>n+p.length,0)<=15000 ? sequence(paths,head,opts):null;
      let planned=paths;
      if(next){planned=next.paths;report.removed+=next.removed;ops.push({...first,paths:planned});}
      else{ops.push(...group);report.oversize++;}
      report.before+=travel(paths,oldHead);oldHead=paths.at(-1).at(-1);
      report.after+=travel(planned,head);head=planned.at(-1).at(-1);
      i=end;
    }
    return {job:{...job,operations:ops,paths:ops.flatMap(op=>op.paths)},report};
  }
  const api={plan,canonical};root.CNCLaserOrder=api;
  if(typeof module!=='undefined'&&module.exports)module.exports=api;
  if(typeof document==='undefined')return;
  const host=document.createElement('section');
  host.innerHTML=`<h3>Laser cut order</h3>
    <label class="check"><input id="laser-order-enabled" type="checkbox">Plan compatible line cuts within each layer</label>
    <label class="check"><input id="laser-order-nearest" type="checkbox">Minimise travel</label>
    <label class="check"><input id="laser-order-inside" type="checkbox">Inside before outside</label>
    <label class="check"><input id="laser-order-dedupe" type="checkbox">Remove exact duplicate paths</label>
    <p class="hint" id="laser-order-report" role="status"></p>`;
  document.getElementById('designLayersPanel').append(host);
  const el=k=>document.getElementById('laser-order-'+k),defaults={enabled:false,nearest:true,inside:true,dedupe:false};
  const settings=()=>({...defaults,...window.projectExtras?.laserOrder});
  const report=txt=>{el('report').textContent=txt;};
  const base=project;
  project=function(){
    const job=base(), opts=settings();
    if(!opts.enabled || job.mode!=='laser'){report(opts.enabled?'Select the laser head to plan cut order.':'Original contour order.');return job;}
    const catalog=window.CNCDesignLayers.layers(),ids=[];
    for(const row of catalog)if(row.output)for(const o of objects)if(o.output!==false && window.CNCDesignLayers.layerFor(o,catalog).id===row.id)ids.push(row.id);
    const result=plan(job,ids,opts),r=result.report;
    report('Nominal travel from XY zero: '+r.before.toFixed(1)+' → '+r.after.toFixed(1)+' mm. Removed '+r.removed+' duplicate(s). '+r.skipped+' protected operation(s). '+(r.oversize?r.oversize+' group(s) exceeded the planning limit and kept their original order.':''));
    return result.job;
  };
  function sync(){
    const config=settings();for(const key of Object.keys(defaults))el(key).checked=config[key]===true;
    for(const k of ['nearest','inside','dedupe'])el(k).disabled=!config.enabled;
    project();
  }
  for(const key of Object.keys(defaults))el(key).onchange=()=>{
    window.projectExtras.laserOrder={...settings(),[key]:el(key).checked};
    invalidate();sync();window.dispatchEvent(new Event('cnc-project-changed'));
  };
  root.addEventListener('cnc-project-loaded',sync);sync();
})(globalThis);
