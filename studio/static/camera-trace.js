/* Threshold contours and skeleton centrelines. Pure geometry also runs in Node. */
(function(root){
 'use strict';
 const MAX_SIDE=384,MAX_EDGES=100000,MAX_VERTICES=20000,MAX_PATHS=1000;
 function threshold(data,width,height,level=128,invert=false){
  if(!Number.isInteger(width)||!Number.isInteger(height)||width<1||height<1||width>MAX_SIDE||height>MAX_SIDE||data.length!==width*height*4)throw Error('Trace input must be RGBA, at most 384 × 384 pixels');
  if(!Number.isFinite(level)||level<0||level>255)throw Error('Threshold must be 0–255');
  const mask=new Uint8Array(width*height);
  for(let i=0;i<mask.length;i++){const a=data[i*4+3]/255,v=(.2126*data[i*4]+.7152*data[i*4+1]+.0722*data[i*4+2])*a+255*(1-a);mask[i]=Number(invert?v>level:v<level);}
  return mask;
 }
 function signedArea(path){let area=0;for(let i=1;i<path.length;i++)area+=path[i-1][0]*path[i][1]-path[i][0]*path[i-1][1];return area/2;}
 function simplify(path){
  // Only remove exactly collinear points. This preserves pixel geometry and
  // narrow gaps, unlike a smoothing tolerance that can cross adjacent contours.
  const p=path.slice(0,-1),result=[];
  for(let i=0;i<p.length;i++){const a=p[(i+p.length-1)%p.length],b=p[i],c=p[(i+1)%p.length];if((b[0]-a[0])*(c[1]-b[1])!==(b[1]-a[1])*(c[0]-b[0]))result.push(b);}
  if(result.length<3)return [];
  return [...result,[...result[0]]];
 }
 function contours(mask,width,height,minArea=4){
  if(!Number.isInteger(width)||!Number.isInteger(height)||width<1||height<1||width>MAX_SIDE||height>MAX_SIDE||mask.length!==width*height)throw Error('Invalid trace mask size');
  if(!Number.isFinite(minArea)||minArea<0||minArea>1000)throw Error('Minimum feature area must be 0–1000 pixels');
  const edges=[],starts=new Map(),stride=width+1;
  function edge(x,y,nx,ny,direction){
   if(edges.length>=MAX_EDGES)throw Error('Image is too noisy to trace; adjust threshold or use a cleaner image');
   const id=edges.length,key=y*stride+x;edges.push({x,y,nx,ny,direction,used:false});if(!starts.has(key))starts.set(key,[]);starts.get(key).push(id);
  }
  function filled(x,y){return x>=0&&y>=0&&x<width&&y<height&&mask[y*width+x];}
  for(let y=0;y<height;y++)for(let x=0;x<width;x++)if(filled(x,y)){
   if(!filled(x,y-1))edge(x,y,x+1,y,0);
   if(!filled(x+1,y))edge(x+1,y,x+1,y+1,1);
   if(!filled(x,y+1))edge(x+1,y+1,x,y+1,2);
   if(!filled(x-1,y))edge(x,y+1,x,y,3);
  }
  const paths=[];let vertices=0;
  for(let initial=0;initial<edges.length;initial++){
   if(edges[initial].used)continue;
   const first=edges[initial],path=[[first.x,first.y]];let id=initial;
   for(let steps=0;steps<=edges.length;steps++){
    const e=edges[id];if(e.used)throw Error('Ambiguous contour; use a cleaner image');e.used=true;path.push([e.nx,e.ny]);
    if(e.nx===first.x&&e.ny===first.y)break;
    const options=(starts.get(e.ny*stride+e.nx)||[]).filter(i=>!edges[i].used);
    if(!options.length)throw Error('Open contour detected');
    // At diagonal pixel contacts turn right first, keeping components separate.
    const rank={1:0,0:1,3:2,2:3};options.sort((a,b)=>rank[(edges[a].direction-e.direction+4)%4]-rank[(edges[b].direction-e.direction+4)%4]);id=options[0];
   }
   const reduced=simplify(path);
   if(reduced.length&&Math.abs(signedArea(reduced))>=minArea){vertices+=reduced.length;if(vertices>MAX_VERTICES||paths.length>=MAX_PATHS)throw Error('Trace exceeds 1,000 contours or 20,000 vertices; remove noise or small features');paths.push(reduced);}
  }
  return paths;
 }
 function skeletonize(mask,width,height){
  if(!Number.isInteger(width)||!Number.isInteger(height)||width<1||height<1||width>MAX_SIDE||height>MAX_SIDE||mask.length!==width*height)throw Error('Invalid skeleton mask size');
  const pixels=Uint8Array.from(mask,v=>Number(!!v)),remove=[];
  const at=(x,y)=>x>=0&&y>=0&&x<width&&y<height?pixels[y*width+x]:0;
  // Zhang–Suen thinning: delete each subiteration simultaneously, preserving
  // endpoints and connectivity. Explicit bounds prevent unbounded processing.
  for(let iteration=0;iteration<MAX_SIDE;iteration++){
   let changed=false;
   for(let phase=0;phase<2;phase++){
    remove.length=0;
    for(let y=0;y<height;y++)for(let x=0;x<width;x++)if(at(x,y)){
     const p=[at(x,y-1),at(x+1,y-1),at(x+1,y),at(x+1,y+1),at(x,y+1),at(x-1,y+1),at(x-1,y),at(x-1,y-1)];
     const count=p.reduce((n,v)=>n+v,0);if(count<2||count>6)continue;
     let transitions=0;for(let i=0;i<8;i++)if(!p[i]&&p[(i+1)%8])transitions++;
     if(transitions!==1)continue;
     const blocked=phase===0?(p[0]*p[2]*p[4]||p[2]*p[4]*p[6]):(p[0]*p[2]*p[6]||p[0]*p[4]*p[6]);
     if(!blocked)remove.push(y*width+x);
    }
    if(remove.length)changed=true;for(const i of remove)pixels[i]=0;
   }
   if(!changed)return pixels;
  }
  throw Error('Skeleton iteration limit reached; simplify the source image');
 }
 function centerlines(mask,width,height,minLength=4){
  if(!Number.isFinite(minLength)||minLength<0||minLength>1000)throw Error('Minimum stroke length must be 0–1000 pixels');
  const pixels=skeletonize(mask,width,height),graph=new Map();let edges=0;
  const filled=(x,y)=>x>=0&&y>=0&&x<width&&y<height&&pixels[y*width+x];
  for(let y=0;y<height;y++)for(let x=0;x<width;x++)if(filled(x,y)){
   const neighbors=[];
   for(let dy=-1;dy<=1;dy++)for(let dx=-1;dx<=1;dx++)if((dx||dy)&&filled(x+dx,y+dy)){
    // Avoid redundant diagonal edges across an existing orthogonal bridge.
    if(dx&&dy&&(filled(x+dx,y)||filled(x,y+dy)))continue;
    neighbors.push((y+dy)*width+x+dx);
   }
   edges+=neighbors.length;if(edges>MAX_EDGES*2)throw Error('Skeleton is too noisy; adjust threshold or clean the source');
   if(neighbors.length)graph.set(y*width+x,neighbors);
  }
  const visited=new Set(),paths=[];let vertices=0;
  const edgeKey=(a,b)=>Math.min(a,b)*width*height+Math.max(a,b);
  function stroke(first,next){
   const ids=[first];let previous=first,current=next;
   for(let i=0;i<=edges;i++){
    const key=edgeKey(previous,current);if(visited.has(key))break;visited.add(key);ids.push(current);
    const neighbors=graph.get(current);if(current===first||neighbors.length!==2)break;
    const following=neighbors.find(v=>v!==previous);previous=current;current=following;
   }
   const path=ids.map(i=>[i%width+.5,Math.floor(i/width)+.5]);
   let length=0;for(let i=1;i<path.length;i++)length+=Math.hypot(path[i][0]-path[i-1][0],path[i][1]-path[i-1][1]);
   if(length<minLength||path.length<2)return;
   // Open strokes retain both endpoints; only genuine graph cycles are closed.
   const reduced=[path[0]];
   for(let i=1;i<path.length-1;i++){const a=path[i-1],b=path[i],c=path[i+1];if((b[0]-a[0])*(c[1]-b[1])!==(b[1]-a[1])*(c[0]-b[0]))reduced.push(b);}
   reduced.push(path.at(-1));vertices+=reduced.length;
   if(vertices>MAX_VERTICES||paths.length>=MAX_PATHS)throw Error('Skeleton exceeds 1,000 strokes or 20,000 vertices; remove noise');paths.push(reduced);
  }
  for(const [node,neighbors]of graph)if(neighbors.length!==2)for(const neighbor of neighbors)if(!visited.has(edgeKey(node,neighbor)))stroke(node,neighbor);
  // Remaining degree-two components are loops, traced once rather than lost.
  for(const [node,neighbors]of graph)for(const neighbor of neighbors)if(!visited.has(edgeKey(node,neighbor)))stroke(node,neighbor);
  return paths;
 }
 function mapPaths(paths,pixelWidth,pixelHeight,width,height,x=0,y=0){
  if(![pixelWidth,pixelHeight,width,height,x,y].every(Number.isFinite)||pixelWidth<=0||pixelHeight<=0||width<=0||height<=0||width>2000||height>2000||x<0||y<0)throw Error('Enter positive physical dimensions and a nonnegative placement');
  return paths.map(p=>p.map(([px,py])=>[x+px/pixelWidth*width,y+(pixelHeight-py)/pixelHeight*height]));
 }
 const exported={threshold,contours,skeletonize,centerlines,mapPaths,simplify,signedArea};
 if(typeof module!=='undefined'&&module.exports)module.exports=exported;
 root.CNCTrace=exported;
 if(typeof document==='undefined')return;
 const panel=document.createElement('details');panel.id='cameraTracePanel';
 panel.innerHTML=`<summary>Image &amp; camera line tracing</summary>
 <p class="hint">Choose shape outlines or stroke centrelines. Centrelines use pixel skeletonization, split at junctions, and keep open strokes open. Use Camera bed registration below to place a trace from four measured landmarks.</p>
 <label>Trace mode<select id="traceMode"><option value="outline">Closed shape outlines</option><option value="centerline">Stroke centrelines</option></select></label>
 <label>Image file<input id="traceImage" type="file" accept="image/png,image/jpeg,image/webp,image/bmp"></label>
 <div class="buttons"><button id="traceCamera">Open camera</button><button id="traceCapture" disabled>Capture frame</button><button id="traceStop" disabled>Stop camera</button></div>
 <video id="traceVideo" autoplay muted playsinline hidden style="width:100%;max-height:240px"></video>
 <div class="pair"><label>Threshold · 0–255<input id="traceThreshold" type="number" min="0" max="255" value="128"></label><label><span id="traceAreaLabel">Minimum feature · pixels²</span><input id="traceArea" type="number" min="0" max="1000" value="4"></label><label>Image width · mm<input id="traceWidth" type="number" min=".1" max="2000" value="100"></label><label>Image height · mm<input id="traceHeight" type="number" min=".1" max="2000" value="75"></label></div>
 <label><input id="traceInvert" type="checkbox"> Trace light shapes instead</label>
 <p class="hint">Dimensions map the entire image rectangle to millimetres. Set both explicitly; changing their ratio stretches the trace. Filtering removes small holes in outline mode or short branches in centreline mode. Input is reduced to at most 384 pixels per side; collinear vertices are simplified without smoothing. Inspect pixel-scale branches and endpoints before machining.</p>
 <button id="tracePreview">Preview trace</button><button id="traceAdd" disabled>Add preview to project</button>
 <canvas id="traceCanvas" width="320" height="240" style="width:100%;background:white" aria-label="Traced vector contour preview"></canvas><p id="traceMessage" role="status">Choose an image or capture a camera frame.</p>`;
 const host=document.querySelector('main > aside');if(!host)return;host.append(panel);
 const el=id=>document.getElementById(id);let stream=null,epoch=0,source=null,traced=null,sourceName='Camera';
 function tell(text){el('traceMessage').textContent=text;}
 function stale(){traced=null;el('traceAdd').disabled=true;window.dispatchEvent(new Event('cnc-trace-changed'));}
 function handle(fn){return async()=>{try{await fn();}catch(error){tell(error.message||'Image tracing failed');}};}
 function stopCamera(){epoch++;if(stream)for(const track of stream.getTracks())track.stop();stream=null;el('traceVideo').srcObject=null;el('traceVideo').hidden=true;el('traceCapture').disabled=el('traceStop').disabled=true;el('traceCamera').disabled=false;}
 function loadPixels(image,width,height,name){
  if(width<1||height<1||width*height>40000000)throw Error('Image dimensions exceed the 40 megapixel limit');
  const scale=Math.min(1,MAX_SIDE/Math.max(width,height)),canvas=document.createElement('canvas');canvas.width=Math.max(1,Math.round(width*scale));canvas.height=Math.max(1,Math.round(height*scale));
  const ctx=canvas.getContext('2d',{willReadFrequently:true});ctx.fillStyle='white';ctx.fillRect(0,0,canvas.width,canvas.height);ctx.drawImage(image,0,0,canvas.width,canvas.height);source=ctx.getImageData(0,0,canvas.width,canvas.height);sourceName=name;stale();
  window.dispatchEvent(new Event('cnc-trace-source'));
  const preview=el('traceCanvas');preview.width=canvas.width;preview.height=canvas.height;preview.getContext('2d').drawImage(canvas,0,0);tell(`Loaded ${canvas.width} × ${canvas.height} pixels. Set physical dimensions, then preview.`);
 }
 Object.assign(exported,{source:()=>source,result:()=>traced,label:()=>sourceName,loadFixture:canvas=>{stopCamera();loadPixels(canvas,canvas.width,canvas.height,'Registration teaching card');}});
 el('traceImage').onchange=handle(async()=>{const file=el('traceImage').files[0];if(!file)return;stopCamera();if(file.size>8*1024*1024)throw Error('Image exceeds 8 MB');if(!/^image\/(png|jpeg|webp|bmp|x-ms-bmp)$/.test(file.type))throw Error('Choose a PNG, JPEG, WebP or BMP image');const bitmap=await createImageBitmap(file);try{loadPixels(bitmap,bitmap.width,bitmap.height,file.name);}finally{bitmap.close();}});
 el('traceCamera').onclick=handle(async()=>{
  if(!window.isSecureContext||!navigator.mediaDevices?.getUserMedia)throw Error('Camera access needs a supported browser on HTTPS or localhost. Image upload also works without a camera.');
  stopCamera();const requestEpoch=epoch;el('traceCamera').disabled=true;tell('Waiting for camera permission…');
  let opened;
  try{opened=await navigator.mediaDevices.getUserMedia({video:{facingMode:{ideal:'environment'},width:{ideal:1280},height:{ideal:720}},audio:false});if(epoch!==requestEpoch||!panel.open){for(const track of opened.getTracks())track.stop();return;}stream=opened;const video=el('traceVideo');video.srcObject=stream;video.hidden=false;await video.play();if(epoch!==requestEpoch)return;el('traceCapture').disabled=false;el('traceStop').disabled=false;tell('Camera open. Capture freezes one image and releases the camera.');}
  catch(error){if(opened)for(const track of opened.getTracks())track.stop();if(epoch===requestEpoch)stopCamera();throw Error('Camera unavailable or permission denied. '+error.message);}
 });
 el('traceCapture').onclick=handle(()=>{const video=el('traceVideo');try{if(!stream||!video.videoWidth)throw Error('Wait for a camera frame');loadPixels(video,video.videoWidth,video.videoHeight,'Camera capture');}finally{stopCamera();}});
 el('traceStop').onclick=()=>{stopCamera();tell('Camera stopped.');};
 panel.addEventListener('toggle',()=>{if(!panel.open)stopCamera();});window.addEventListener('pagehide',stopCamera);window.addEventListener('beforeunload',stopCamera);
 for(const id of ['traceThreshold','traceArea','traceWidth','traceHeight','traceInvert','traceMode'])el(id).addEventListener('input',stale);
 el('traceMode').addEventListener('change',()=>{stale();el('traceAreaLabel').textContent=el('traceMode').value==='centerline'?'Minimum stroke length · pixels':'Minimum feature · pixels²';});
 el('tracePreview').onclick=handle(()=>{
  stale();if(!source)throw Error('Choose an image or capture a frame first');const mask=threshold(source.data,source.width,source.height,Number(el('traceThreshold').value),el('traceInvert').checked),mode=el('traceMode').value,paths=(mode==='centerline'?centerlines:contours)(mask,source.width,source.height,Number(el('traceArea').value));if(!paths.length)throw Error('No paths found. Adjust threshold, inversion or minimum feature setting.');
  const width=Number(el('traceWidth').value),height=Number(el('traceHeight').value);mapPaths(paths,source.width,source.height,width,height);traced={paths,width,height,mode};const canvas=el('traceCanvas'),ctx=canvas.getContext('2d');ctx.fillStyle='white';ctx.fillRect(0,0,canvas.width,canvas.height);ctx.strokeStyle='#176a9a';ctx.lineWidth=.7;for(const path of paths){ctx.beginPath();path.forEach(([x,y],i)=>i?ctx.lineTo(x,y):ctx.moveTo(x,y));ctx.stroke();}el('traceAdd').disabled=false;window.dispatchEvent(new Event('cnc-trace-changed'));tell(`${paths.length} ${mode==='centerline'?'centreline strokes':'closed contours'} · ${paths.reduce((n,p)=>n+p.length,0)} vertices · ${width} × ${height} mm image area. Inspect before adding.`);
 });
 el('traceAdd').onclick=handle(()=>{if(!traced||!source)throw Error('Preview the trace first');const x=Number(document.getElementById('shapeX').value),y=Number(document.getElementById('shapeY').value),paths=mapPaths(traced.paths,source.width,source.height,traced.width,traced.height,x,y);addGenerated((traced.mode==='centerline'?'Image centrelines · ':'Image contours · ')+sourceName,paths);tell('Added editable trace geometry at the design X/Y placement. Open centrelines need a line engraving operation. Inspect before generating a job.');});
})(typeof globalThis!=='undefined'?globalThis:this);
