/* Register a frozen, planar camera image with measured XY landmarks. */
((root)=>{
  'use strict';
  const validPoint=p=>Array.isArray(p)&&p.length===2&&p.every(Number.isFinite);
  const cross=(a,b,c)=>(b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0]);
  function center(points){return [0,1].map(k=>points.reduce((s,p)=>s+p[k],0)/points.length);}
  function convex(points) {
    const scale=Math.max(...points.flatMap(a=>points.map(b=>Math.hypot(a[0]-b[0],a[1]-b[1]))));
    const turns=points.map((p,i)=>cross(p,points[(i+1)%4],points[(i+2)%4]));
    if(!scale||turns.some(v=>Math.abs(v)<scale*scale*1e-6)||turns.some(v=>Math.sign(v)!==Math.sign(turns[0])))throw Error('Use four well-separated landmarks around a convex region; check the correspondence order.');
  }
  function inside(p,quad,tol=1e-9){
    const signs=quad.map((a,i)=>cross(a,quad[(i+1)%4],p));
    return signs.every(v=>v>=-tol)||signs.every(v=>v<=tol);
  }
  function fit(from,to) {
    // Centering keeps h33 nonzero for a valid finite calibration region.
    const a=center(from), b=center(to);
    const scale=list=>Math.max(...list.map(p=>Math.hypot(...p)));
    const unit=(p,o)=>[p[0]-o[0],p[1]-o[1]];
    const A=scale(from.map(p=>unit(p,a))),B=scale(to.map(p=>unit(p,b)));
    let rows=[];
    for(let i=0;i<from.length;i++){
      const [x,y]=unit(from[i],a).map(n=>n/A),[u,v]=unit(to[i],b).map(n=>n/B);
      rows.push([x,y,1,0,0,0,-u*x,-u*y,u],[0,0,0,x,y,1,-v*x,-v*y,v]);
    }
    if(rows.length>8){
      const normal=Array.from({length:8},()=>Array(9).fill(0));
      for(const row of rows)for(let i=0;i<8;i++)for(let j=0;j<=8;j++)normal[i][j]+=row[i]*row[j];
      rows=normal;
    }
    for(let j=0;j<8;j++){
      let pivot=j;for(let i=j+1;i<8;i++)if(Math.abs(rows[i][j])>Math.abs(rows[pivot][j]))pivot=i;
      if(Math.abs(rows[pivot][j])<1e-10)throw Error('Landmarks produce an unstable calibration.');
      [rows[j],rows[pivot]]=[rows[pivot],rows[j]];
      const d=rows[j][j];for(let k=j;k<=8;k++)rows[j][k]/=d;
      for(let i=0;i<8;i++)if(i!==j){const f=rows[i][j];for(let k=j;k<=8;k++)rows[i][k]-=f*rows[j][k];}
    }
    return {a,b,A,B,h:rows.map(r=>r[8])};
  }
  function apply(H,p) {
    const x=(p[0]-H.a[0])/H.A, y=(p[1]-H.a[1])/H.A, h=H.h,d=1+h[6]*x+h[7]*y;
    if(!Number.isFinite(d)||Math.abs(d)<1e-9)throw Error('Point is on the perspective horizon.');
    return [H.b[0]+H.B*(h[0]*x+h[1]*y+h[2])/d,H.b[1]+H.B*(h[3]*x+h[4]*y+h[5])/d];
  }
  function inverseHomography(H){
    const [a,b,c,d,e,f,g,h]=H.h;
    const cof=[e-f*h,c*h-b,b*f-c*e,f*g-d,a-c*g,c*d-a*f,d*h-e*g,b*g-a*h,a*e-b*d];
    if(Math.abs(cof[8])<1e-10)throw Error('Landmarks produce an unstable inverse mapping.');
    return {a:H.b,b:H.a,A:H.B,B:H.A,h:cof.slice(0,8).map(n=>n/cof[8])};
  }
  // One-coefficient division model about the image centre, in square-pixel units.
  function radial(p,k,aspect,inverse=false){
    if(!k)return p;
    const x=p[0]-.5,y=p[1]-.5;
    const r2=4*(x*x*aspect*aspect+y*y)/(aspect*aspect+1);
    let factor=1/(1+k*r2);
    if(inverse){
      const disc=1-4*k*r2;
      if(disc<0)throw Error('Location is outside the invertible lens field.');
      factor=2/(1+Math.sqrt(disc));
    }
    return [.5+factor*x,.5+factor*y];
  }
  const toWork=(cal,uv)=>apply(cal.forward,radial(uv,cal.k,cal.aspect));
  const toImage=(cal,xy)=>radial(apply(cal.inverse,xy),cal.k,cal.aspect,true);
  function calibrate(points,bed,options={}){
    if(!validPoint(bed)||bed.some(n=>n<=0||n>2000))throw Error('Set valid machine bed dimensions.');
    const lens=!!options.radial,count=lens?9:4,aspect=options.aspect??1,tolerance=options.tolerance??1;
    if(!Array.isArray(points)||points.length!==count||points.some(p=>!validPoint(p?.image)||!validPoint(p?.work)))
      throw Error('Set image and work XY for '+(lens?'all nine':'four')+' landmarks.');
    for(const p of points)if(p.image.some(n=>n<0||n>1)||p.work.some((n,i)=>n<0||n>bed[i]))throw Error('Landmarks must lie inside the image and machine bed.');
    const outer=points.slice(0,4),mean=center(outer.map(p=>p.image));
    const angle=p=>Math.atan2(p.image[1]-mean[1],p.image[0]-mean[0]);
    const order=outer.slice().sort((a,b)=>angle(a)-angle(b)),image=order.map(p=>p.image),work=order.map(p=>p.work);
    convex(image);convex(work);
    if(!lens)return {image,work,forward:fit(image,work),inverse:fit(work,image),k:0};
    if(!Number.isFinite(aspect)||aspect<.05||aspect>20||!Number.isFinite(tolerance)||tolerance<.01||tolerance>20)
      throw Error('Choose an image and a permitted error from 0.01 to 20 mm.');
    for(let i=0;i<points.length;i++){
      if(i>=4&&!inside(points[i].work,work))throw Error('Put the additional measured targets inside the four outer work points.');
      for(let j=0;j<i;j++)if(Math.hypot(...points[i].image.map((n,z)=>n-points[j].image[z]))<.005)
        throw Error('Use nine distinct, well-separated targets.');
    }
    for(let axis=0;axis<2;axis++){
      const extent=values=>Math.max(...values)-Math.min(...values);
      const outerSpan=extent(image.map(p=>p[axis])),innerSpan=extent(points.slice(4).map(p=>p.image[axis]));
      if(innerSpan<outerSpan*.35)throw Error('Spread the five extra targets across the interior and near all four sides.');
    }
    const targets=points.map(p=>p.work);
    function score(k){
      const corrected=points.map(p=>radial(p.image,k,aspect));
      const forward=fit(corrected,targets);
      const errors=corrected.map((p,i)=>{const xy=apply(forward,p); return Math.hypot(xy[0]-targets[i][0],xy[1]-targets[i][1]);});
      return {k,forward,errors,rms:Math.sqrt(errors.reduce((sum,r)=>sum+r*r,0)/count)};
    }
    // Search the invertible, monotonic range, then refine around the best coarse fit.
    const limit=.65,step=limit/24;
    let best=score(0);
    for(let i=-24;i<=24;i++){const trial=score(i*step);if(trial.rms<best.rms)best=trial;}
    if(Math.abs(best.k)>=limit-1e-10)throw Error('Lens fit reached the supported range. Recheck targets, framing and optical centre.');
    let lo=Math.max(-limit,best.k-step),hi=Math.min(limit,best.k+step);
    for(let i=0;i<40;i++){
      const left=score((2*lo+hi)/3),right=score((lo+2*hi)/3);
      if(left.rms<=right.rms){hi=right.k;if(left.rms<best.rms)best=left;}
      else{lo=left.k;if(right.rms<best.rms)best=right;}
    }
    const probe=score(best.k+(best.k>0?-.04:.04));
    const sensitivity=Math.sqrt(Math.max(0,probe.rms*probe.rms-best.rms*best.rms))/.04;
    const region=Math.max(...work.flatMap(p=>work.map(q=>Math.hypot(p[0]-q[0],p[1]-q[1]))));
    if(sensitivity<.002*region)throw Error('Targets do not constrain the lens coefficient. Spread markers across the centre, edges and corners.');
    const worst=Math.max(...best.errors),worstIndex=best.errors.indexOf(worst);
    if(worst>tolerance)throw Error('Target '+(worstIndex+1)+' misses by '+worst.toFixed(3)+' mm (allowed '+tolerance+' mm). Recheck measured pairs.');
    const cal={image,work,k:best.k,aspect,forward:best.forward,inverse:inverseHomography(best.forward),rms:best.rms,worst,tolerance};
    // The fitted mapping must cover the work quadrilateral without a horizon or a folded lens inverse.
    const denominators=work.map(p=>1+cal.inverse.h[6]*(p[0]-cal.inverse.a[0])/cal.inverse.A+cal.inverse.h[7]*(p[1]-cal.inverse.a[1])/cal.inverse.A);
    if(denominators.some(n=>n<1e-8))throw Error('Landmarks put the work area across a perspective horizon.');
    for(const xy of work)toImage(cal,xy);
    return cal;
  }
  function place(paths,dimensions,cal,bed){
    let count=0;
    const measured=p=>{
      if(!cal.aspect&&!inside(p,cal.image))throw Error('A traced point lies outside the calibrated region. Use landmarks surrounding the artwork.');
      const q=toWork(cal,p);
      if(!inside(q,cal.work,1e-6)||q.some((n,i)=>n<-.000001||n>bed[i]+.000001))throw Error('A traced point lies outside the calibrated work region.');
      return q.map((n,i)=>Math.min(bed[i],Math.max(0,n)));
    };
    return paths.map(path=>{
      if(!path.length)return [];
      const uv=path.map(([x,y])=>[x/dimensions[0],y/dimensions[1]]);
      const result=[measured(uv[0])];
      function segment(a,b,first,last,depth){
        if(cal.aspect&&cal.k){
          const sample=t=>a.map((n,k)=>n+t*(b[k]-n));
          const mid=sample(.5),q=measured(mid),dx=last[0]-first[0],dy=last[1]-first[1],len=Math.hypot(dx,dy);
          const error=p=>len?Math.abs(dx*(p[1]-first[1])-dy*(p[0]-first[0]))/len:Math.hypot(p[0]-first[0],p[1]-first[1]);
          if(Math.max(error(q),error(measured(sample(.25))),error(measured(sample(.75))))>.025){
            if(depth>=12)throw Error('Simplify this trace or reduce the calibration region.');
            segment(a,mid,first,q,depth+1);segment(mid,b,q,last,depth+1);return;
          }
        }
        if(++count>100000)throw Error('Simplify the registered trace to 100000 points.');
        result.push(last);
      }
      for(let i=1;i<uv.length;i++)segment(uv[i-1],uv[i],result[result.length-1],measured(uv[i]),0);
      return result;
    });
  }
  const api={calibrate,apply,inside,place,toWork,toImage};root.CNCCameraRegister=api;
  if(typeof module!=='undefined'&&module.exports)module.exports=api;
  if(typeof document==='undefined')return;
  const byId=id=>document.getElementById(id), trace=root.CNCTrace;
  const section=document.createElement('details');section.className='tab-section';section.open=true;
  section.innerHTML=`<summary>Camera bed registration</summary><p class="hint">Pick a target on the frozen image and enter its measured XY on the work plane. Surround the artwork with the first four targets. For lens correction, add five targets spread across the interior and near the sides.</p>
    <label>Camera model<select id="camera-reg-model"><option value="perspective">Perspective · four targets</option><option value="radial">Perspective and radial lens · nine targets</option></select></label>
    <label id="camera-reg-limit-row" hidden>Maximum target error · mm<input id="camera-reg-limit" type="number" value="1" min=".01" max="20" step="any"></label>
    <div class="buttons"><button id="camera-reg-demo">Try registration card</button><button id="camera-reg-lens-demo">Try lens calibration card</button></div>
    <label>Marker<select id="camera-reg-marker"><option value="0">1</option><option value="1">2</option><option value="2">3</option><option value="3">4</option><option value="4">Independent check point</option></select></label>
    <canvas id="camera-reg-image" width="300" height="180" style="width:100%;height:auto;cursor:crosshair;touch-action:none" aria-label="Frozen camera image with calibration markers"></canvas>
    <div class="pair"><label>Image X · px<input id="camera-reg-px" type="number" step="any"></label><label>Image Y · px<input id="camera-reg-py" type="number" step="any"></label>
    <label>Work X · mm<input id="camera-reg-wx" type="number" step="any"></label><label>Work Y · mm<input id="camera-reg-wy" type="number" step="any"></label></div>
    <div class="buttons"><button id="camera-reg-fit">Solve registration</button><button id="camera-reg-save">Save for this profile</button>
    <button id="camera-reg-recall">Recall profile calibration</button><button id="camera-reg-check">Measure check error</button></div>
    <label>Bed overlay opacity<input id="camera-reg-opacity" type="range" value="45" min="0" max="100"></label>
    <button id="camera-reg-add">Place previewed trace on bed</button><p id="camera-reg-info" role="status">Choose a trace image, capture a frame or try the teaching card.</p>`;
  byId('design-panel-images').append(section);
  const el=k=>byId('camera-reg-'+k), streamImage=()=>trace.source(), bed=()=>['width','height'].map(k=>byId(k).valueAsNumber);
  const note=s=>{el('info').textContent=s;};
  const run=fn=>()=>{try{fn();}catch(err){note(err.message);}};
  const key=()=>JSON.stringify(['cnc-camera-registration-v1',window.studioSession?.username||'',byId('profile').value]);
  const overlay=document.createElementNS('http://www.w3.org/2000/svg','image');
  overlay.style.pointerEvents='none';overlay.setAttribute('preserveAspectRatio','none');overlay.setAttribute('aria-hidden','true');byId('stock').after(overlay);
  let pairs=[], active=null;
  const marker=()=>Number(el('marker').value), lensMode=()=>el('model').value==='radial';
  function updateMode(){
    const select=el('marker'), selected=select.value;
    const indices=lensMode()?[0,1,2,3,5,6,7,8,9,4]:[0,1,2,3,4];
    select.replaceChildren(...indices.map(i=>new Option(i===4?'Independent check point':String(i<4?i+1:i),String(i))));
    select.value=indices.includes(Number(selected))?selected:'0';
    el('limit-row').hidden=!lensMode();
  }
  function reset(){
    const [w,h]=bed();
    pairs=[[0,h],[w,h],[w,0],[0,0],[w/2,h/2]].map(work=>({work,image:null}));
    for(let i=0;i<5;i++)pairs.push({work:null,image:null});
    updateMode();el('marker').value='0';clearFit();fields();paint();
  }
  function clearFit(){active=null;overlay.removeAttribute('href');buttons();}
  function buttons(){el('save').disabled=!active;el('check').disabled=!active;el('add').disabled=!active||!trace.result();}
  function fields(){
    const p=pairs[marker()],s=streamImage();
    el('px').value=p.image&&s?+(p.image[0]*s.width).toFixed(3):'';
    el('py').value=p.image&&s?+(p.image[1]*s.height).toFixed(3):'';
    el('wx').value=p.work?.[0]??'';el('wy').value=p.work?.[1]??'';
  }
  function paint(){
    const s=streamImage(),can=el('image'),ctx=can.getContext('2d');ctx.clearRect(0,0,can.width,can.height);if(!s)return;
    can.width=s.width;can.height=s.height;ctx.putImageData(s,0,0);
    pairs.forEach(({image},i)=>{if(!validPoint(image)||(!lensMode()&&i>4))return;const x=image[0]*s.width,y=image[1]*s.height;
      ctx.strokeStyle=i===marker()?'#e34d1c':'#1552ae';ctx.fillStyle=ctx.strokeStyle;ctx.lineWidth=2;ctx.beginPath();ctx.arc(x,y,4,0,2*Math.PI);ctx.stroke();
      ctx.font='bold 12px sans-serif';ctx.fillText(i===4?'✓':String(i<4?i+1:i),x+5,y-4);
    });
  }
  function renderOverlay(){
    const s=streamImage(),[w,h]=bed(),can=document.createElement('canvas'),scale=Math.min(1,750/Math.max(w,h));
    can.width=Math.max(1,Math.round(w*scale));can.height=Math.max(1,Math.round(h*scale));
    const ctx=can.getContext('2d'),dst=ctx.createImageData(can.width,can.height);
    // Inverse map the work plane. Outside the measured quadrilateral stays transparent.
    for(let j=0;j<can.height;j++)for(let i=0;i<can.width;i++){
      const work=[(i+.5)*w/can.width,h-(j+.5)*h/can.height];
      if(!inside(work,active.work))continue;
      const uv=toImage(active,work);
      if(uv.some(v=>v<0||v>1))continue;
      const px=Math.min(s.width-1,Math.floor(uv[0]*s.width)),py=Math.min(s.height-1,Math.floor(uv[1]*s.height));
      const from=4*(py*s.width+px),to=4*(j*can.width+i);
      dst.data[to]=s.data[from];dst.data[to+1]=s.data[from+1];dst.data[to+2]=s.data[from+2];dst.data[to+3]=255;
    }
    ctx.putImageData(dst,0,0);overlay.setAttribute('href',can.toDataURL());overlay.setAttribute('width',w);overlay.setAttribute('height',h);
    overlay.setAttribute('opacity',el('opacity').value/100);
  }
  function calculate(){
    const s=streamImage();
    if(!s)throw Error('Choose an image or capture a frame first.');
    clearFit();
    const samples=lensMode()?pairs.filter((_,i)=>i!==4):pairs.slice(0,4);
    active=calibrate(samples,bed(),{radial:lensMode(),aspect:s.width/s.height,tolerance:el('limit').valueAsNumber});
    renderOverlay();buttons();
    note(lensMode()?'Lens fit using nine measured targets. RMS error '+active.rms.toFixed(3)+' mm · maximum '+active.worst.toFixed(3)+' mm · k '+active.k.toFixed(5)+'. Measure an independent check point.':
      'Registration solved. Check a separate measured point before placing artwork.');
  }
  el('model').onchange=()=>{updateMode();clearFit();fields();paint();note('Camera model changed. Solve the measured targets again.');};
  el('limit').oninput=()=>{clearFit();note('Solve again with the chosen maximum target error.');};
  function changed(){
    if(marker()!==4){clearFit();note('Registration changed. Solve to update the bed overlay.');}
    paint();
  }
  el('marker').onchange=()=>{fields();paint();};
  el('image').onpointerdown=e=>{
    const src=streamImage();if(!src)return;
    const rect=el('image').getBoundingClientRect();
    pairs[marker()].image=[(e.clientX-rect.left)/rect.width,(e.clientY-rect.top)/rect.height].map(n=>Math.max(0,Math.min(1,n)));
    fields();changed();
  };
  for(const k of ['px','py','wx','wy'])el(k).oninput=()=>{
    const s=streamImage(), p=pairs[marker()];
    if(k==='wx'||k==='wy')p.work=[el('wx').valueAsNumber,el('wy').valueAsNumber];
    else if(s)p.image=[el('px').valueAsNumber/s.width,el('py').valueAsNumber/s.height];
    changed();
  };
  el('fit').onclick=run(calculate);
  el('opacity').oninput=()=>overlay.setAttribute('opacity',el('opacity').value/100);
  el('save').onclick=run(()=>{
    const s=streamImage();if(!active||!s)throw Error('Solve the landmarks first.');
    localStorage.setItem(key(),JSON.stringify({version:2,bed:bed(),aspect:s.width/s.height,model:el('model').value,tolerance:el('limit').valueAsNumber,pairs}));
    note('Calibration saved for this browser and the selected machine profile.');
  });
  el('recall').onclick=run(()=>{
    const s=streamImage(), record=JSON.parse(localStorage.getItem(key())||'null');
    if(!s)throw Error('Load a frozen image from the calibrated camera first.');
    if(!record||![1,2].includes(record.version))throw Error('No saved calibration for this profile.');
    if(JSON.stringify(record.bed)!==JSON.stringify(bed())||Math.abs(record.aspect/(s.width/s.height)-1)>.005)throw Error('Bed dimensions or image aspect ratio differ from calibration.');
    const model=record.version===1?'perspective':record.model,old=record.version===1;
    if(!['perspective','radial'].includes(model)||!Array.isArray(record.pairs)||record.pairs.length!==(old?4:10)||record.pairs.some(p=>!p||typeof p!=='object'))
      throw Error('Invalid saved camera calibration.');
    const fitting=model==='radial'?record.pairs.filter((_,i)=>i!==4):record.pairs.slice(0,4);
    calibrate(fitting,bed(),{radial:model==='radial',aspect:s.width/s.height,tolerance:record.tolerance});
    el('model').value=model;el('limit').value=record.tolerance||1;reset();
    pairs.splice(0,record.pairs.length,...record.pairs);fields();paint();calculate();
  });
  el('check').onclick=run(()=>{
    const sample=pairs[4];
    if(!active||!validPoint(sample.image)||!validPoint(sample.work))throw Error('Select Independent check point and set its image and work coordinates inside the measured region.');
    const s=streamImage();
    if(pairs.some((p,i)=>i!==4 && (lensMode()||i<4) && validPoint(p.image) &&
        (Math.hypot((p.image[0]-sample.image[0])*s.width,(p.image[1]-sample.image[1])*s.height)<.5 ||
         Math.hypot(p.work[0]-sample.work[0],p.work[1]-sample.work[1])<1e-7)))
      throw Error('Choose an independent check point different from the fitted targets.');
    const p=toWork(active,sample.image);
    if(!inside(sample.work,active.work)||!inside(p,active.work))throw Error('Select an independent point inside the measured region.');
    const error=Math.hypot(p[0]-sample.work[0],p[1]-sample.work[1]);
    const quality=active.aspect?(error>active.tolerance?' Exceeds the chosen tolerance. Remeasure before using this calibration.':' Within the chosen tolerance.'):'';
    note('Independent check error '+error.toFixed(3)+' mm. Predicted work XY '+p.map(v=>v.toFixed(3)).join(', ')+' mm.'+quality);
  });
  el('add').onclick=run(()=>{
    const s=streamImage(),res=trace.result();if(!s||!res||!active)throw Error('Solve the registration and preview the trace.');
    const paths=place(res.paths,[s.width,s.height],active,bed());addGenerated('Registered trace · '+trace.label(),paths);
    note(paths.length+' registered contours or strokes added in work coordinates.');
  });
  el('demo').onclick=run(()=>{
    const canvas=document.createElement('canvas');canvas.width=240;canvas.height=160;
    const c=canvas.getContext('2d');c.fillStyle='white';c.fillRect(0,0,240,160);c.fillStyle='black';c.fillRect(100,66,40,28);
    const px=[[30,12],[223,23],[207,149],[15,132]];
    c.fillStyle='#49e6cb';for(const [x,y] of px)c.fillRect(x-2,y-2,4,4);
    el('model').value='perspective';trace.loadFixture(canvas);
    px.forEach(([x,y],i)=>{pairs[i].image=[x/240,y/160];});
    fields();paint();calculate();note('Teaching card solved from four turquoise targets. Preview its dark rectangle in Image tracing, then place the trace on the bed.');
  });
  el('lens-demo').onclick=run(()=>{
    const canvas=document.createElement('canvas');canvas.width=240;canvas.height=160;
    const ctx=canvas.getContext('2d'),frame=ctx.createImageData(canvas.width,canvas.height),k=-.32,aspect=1.5;
    for(let y=0;y<canvas.height;y++)for(let x=0;x<canvas.width;x++){
      const uv=radial([(x+.5)/canvas.width,(y+.5)/canvas.height],k,aspect);
      const ink=uv[0]>.32&&uv[0]<.62&&uv[1]>.34&&uv[1]<.58;
      const offset=4*(x+y*canvas.width);
      frame.data.fill(ink?0:255,offset,offset+3);frame.data[offset+3]=255;
    }
    ctx.putImageData(frame,0,0);
    const locations=[[.15,.15],[.85,.15],[.85,.85],[.15,.85],[.65,.62],[.50,.20],[.79,.53],[.47,.76],[.21,.52],[.67,.39]];
    const targets=locations.map(p=>radial(p,k,aspect,true));
    ctx.fillStyle='#49e6cb';for(const uv of targets)ctx.fillRect(uv[0]*canvas.width-2,uv[1]*canvas.height-2,4,4);
    el('model').value='radial';el('limit').value=1;trace.loadFixture(canvas);
    const [w,h]=bed();
    pairs=locations.map((uv,i)=>({image:targets[i],work:[Math.max(0,Math.min(w,(uv[0]-.15)/.70*w)),Math.max(0,Math.min(h,(.85-uv[1])/.70*h))]}));
    updateMode();fields();paint();calculate();
  });
  byId('profile').addEventListener('change',()=>queueMicrotask(reset));
  for(const id of ['width','height'])byId(id).addEventListener('input',reset);
  root.addEventListener('cnc-trace-source',reset);root.addEventListener('cnc-trace-changed',buttons);
  root.addEventListener('cnc-project-loaded',reset);root.studioReady?.then(reset);reset();
})(globalThis);
