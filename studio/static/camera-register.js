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
    const rows=[];
    for(let i=0;i<4;i++){
      const [x,y]=unit(from[i],a).map(n=>n/A),[u,v]=unit(to[i],b).map(n=>n/B);
      rows.push([x,y,1,0,0,0,-u*x,-u*y,u],[0,0,0,x,y,1,-v*x,-v*y,v]);
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
  function calibrate(points,bed) {
    if(!validPoint(bed)||bed.some(n=>n<=0||n>2000))throw Error('Set valid machine bed dimensions.');
    if(!Array.isArray(points)||points.length!==4||points.some(p=>!validPoint(p?.image)||!validPoint(p?.work)))throw Error('Set image and work XY for four landmarks.');
    for(const p of points)if(p.image.some(n=>n<0||n>1)||p.work.some((n,i)=>n<0||n>bed[i]))throw Error('Landmarks must lie inside the image and machine bed.');
    const mean=center(points.map(p=>p.image)),angle=p=>Math.atan2(p.image[1]-mean[1],p.image[0]-mean[0]);
    const order=points.slice().sort((a,b)=>angle(a)-angle(b)),image=order.map(p=>p.image),work=order.map(p=>p.work);
    convex(image);convex(work);
    return {image,work,forward:fit(image,work),inverse:fit(work,image)};
  }
  function place(paths,dimensions,cal,bed){
    return paths.map(path=>path.map(([x,y])=>{
      const p=[x/dimensions[0],y/dimensions[1]];
      if(!inside(p,cal.image))throw Error('A traced point lies outside the calibrated region. Use landmarks surrounding the artwork.');
      const q=apply(cal.forward,p);
      return q.map((n,i)=>Math.min(bed[i],Math.max(0,n)));
    }));
  }
  const api={calibrate,apply,inside,place};root.CNCCameraRegister=api;
  if(typeof module!=='undefined'&&module.exports)module.exports=api;
  if(typeof document==='undefined')return;
  const byId=id=>document.getElementById(id), trace=root.CNCTrace;
  const section=document.createElement('details');section.className='tab-section';section.open=true;
  section.innerHTML=`<summary>Camera bed registration</summary><p class="hint">Set four landmarks around the artwork on the same plane. Pick a marker in the list, click it on the frozen image, then enter its measured work coordinates.</p>
    <button id="camera-reg-demo">Try registration card</button>
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
  const marker=()=>Number(el('marker').value);
  function reset(){
    const [w,h]=bed();
    pairs=[[0,h],[w,h],[w,0],[0,0],[w/2,h/2]].map(work=>({work,image:null}));
    el('marker').value='0';clearFit();fields();paint();
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
    pairs.forEach(({image},i)=>{if(!validPoint(image))return;const x=image[0]*s.width,y=image[1]*s.height;
      ctx.strokeStyle=i===marker()?'#e34d1c':'#1552ae';ctx.fillStyle=ctx.strokeStyle;ctx.lineWidth=2;ctx.beginPath();ctx.arc(x,y,4,0,2*Math.PI);ctx.stroke();
      ctx.font='bold 12px sans-serif';ctx.fillText(i===4?'✓':String(i+1),x+5,y-4);
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
      const uv=apply(active.inverse,work), px=Math.min(s.width-1,Math.max(0,Math.floor(uv[0]*s.width))),py=Math.min(s.height-1,Math.max(0,Math.floor(uv[1]*s.height)));
      const from=4*(py*s.width+px),to=4*(j*can.width+i);
      dst.data[to]=s.data[from];dst.data[to+1]=s.data[from+1];dst.data[to+2]=s.data[from+2];dst.data[to+3]=255;
    }
    ctx.putImageData(dst,0,0);overlay.setAttribute('href',can.toDataURL());overlay.setAttribute('width',w);overlay.setAttribute('height',h);
    overlay.setAttribute('opacity',el('opacity').value/100);
  }
  function calculate(){
    if(!streamImage())throw Error('Choose an image or capture a frame first.');
    clearFit();active=calibrate(pairs.slice(0,4),bed());renderOverlay();buttons();
    note('Registration solved. Check a separate measured point before placing artwork.');
  }
  function changed(){
    if(marker()<4){clearFit();note('Registration changed. Solve to update the bed overlay.');}
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
    localStorage.setItem(key(),JSON.stringify({version:1,bed:bed(),aspect:s.width/s.height,pairs:pairs.slice(0,4)}));
    note('Calibration saved for this browser and the selected machine profile.');
  });
  el('recall').onclick=run(()=>{
    const s=streamImage(), record=JSON.parse(localStorage.getItem(key())||'null');
    if(!s)throw Error('Load a frozen image from the calibrated camera first.');
    if(!record||record.version!==1)throw Error('No saved calibration for this profile.');
    if(JSON.stringify(record.bed)!==JSON.stringify(bed())||Math.abs(record.aspect/(s.width/s.height)-1)>.005)throw Error('Bed dimensions or image aspect ratio differ from calibration.');
    calibrate(record.pairs,bed());pairs.splice(0,4,...record.pairs);fields();paint();calculate();
  });
  el('check').onclick=run(()=>{
    const sample=pairs[4];
    if(!active||!validPoint(sample.image)||!validPoint(sample.work)||!inside(sample.image,active.image))throw Error('Select Independent check point and set its image and work coordinates inside the measured region.');
    const p=apply(active.forward,sample.image);
    note('Independent check error '+Math.hypot(p[0]-sample.work[0],p[1]-sample.work[1]).toFixed(3)+' mm. Predicted work XY '+p.map(v=>v.toFixed(3)).join(', ')+' mm.');
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
    trace.loadFixture(canvas);
    px.forEach(([x,y],i)=>{pairs[i].image=[x/240,y/160];});
    fields();paint();calculate();note('Teaching card solved from four turquoise targets. Preview its dark rectangle in Image tracing, then place the trace on the bed.');
  });
  byId('profile').addEventListener('change',()=>queueMicrotask(reset));
  for(const id of ['width','height'])byId(id).addEventListener('input',reset);
  root.addEventListener('cnc-trace-source',reset);root.addEventListener('cnc-trace-changed',buttons);
  root.addEventListener('cnc-project-loaded',reset);root.studioReady?.then(reset);reset();
})(globalThis);
