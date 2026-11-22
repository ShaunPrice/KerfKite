'use strict';
// Editable inputs are sufficient to reconstruct CAM operations, depths and
// powers. Reuse unchanged object strings across the bounded undo history.
function drawingSnapshot(previous) {
  const settings=JSON.stringify(editorSettings()), layers=JSON.stringify(window.projectExtras?.designLayers);
  let unchanged=previous?.objects.length===objects.length && previous.settings===settings && previous.layers===layers;
  const items=objects.map((object,i)=>{
    const encoded=JSON.stringify(object);
    if(previous && previous.objects[i]===encoded)return previous.objects[i];
    unchanged=false;
    return encoded;
  });
  return unchanged?previous:{objects:items,settings,layers};
}
let snapshots=[drawingSnapshot()],historyIndex=0,restoring=false;
window.recordHistory=()=>{
  if(restoring)return;
  const previous=snapshots[historyIndex],next=drawingSnapshot(previous);
  if(next===previous)return;
  snapshots.splice(historyIndex+1);
  snapshots.push(next);
  if(snapshots.length>40)snapshots.shift();
  historyIndex=snapshots.length-1;
};
function restore(delta){
  const i=historyIndex+delta;
  if(i<0||i>=snapshots.length)return;
  historyIndex=i;
  const s=snapshots[i],settings=JSON.parse(s.settings);
  objects=s.objects.map(text=>JSON.parse(text));
  window.projectExtras??={};
  if(s.layers===undefined)delete window.projectExtras.designLayers;
  else window.projectExtras.designLayers=JSON.parse(s.layers);
  selected=objects.length-1;
  for(const k of fields)if(settings[k]!==undefined)$(k).value=settings[k];
  restoring=true;
  try{invalidate();}finally{restoring=false;}
  modeUI();draw();window.dispatchEvent(new Event('cnc-layers-changed'));
}
$('undo').onclick=()=>restore(-1);$('redo').onclick=()=>restore(1);
function selectedObject(){if(selected<0)throw Error('Select an object');return objects[selected];}
function askNumber(label,initial,min,max){const raw=prompt(label,String(initial));if(raw===null)return null;const n=Number(raw);if(!Number.isFinite(n)||n<min||n>max)throw Error(`Enter a number between ${min} and ${max}`);return n;}
$('rotateObject').onclick=task(()=>{const o=selectedObject(),angle=askNumber('Rotation in degrees',45,-360,360);if(angle===null)return;const[x,y,mx,my]=bounds(o.paths),cx=(x+mx)/2,cy=(y+my)/2,a=angle*Math.PI/180;o.paths=o.paths.map(p=>p.map(([x,y])=>[cx+(x-cx)*Math.cos(a)-(y-cy)*Math.sin(a),cy+(x-cx)*Math.sin(a)+(y-cy)*Math.cos(a)]));invalidate();draw();});
$('mirrorObject').onclick=task(()=>{const o=selectedObject(),[x,,mx]=bounds(o.paths);o.paths=o.paths.map(p=>p.map(([px,py])=>[x+mx-px,py]));invalidate();draw();});
$('arrayObject').onclick=task(()=>{const o=selectedObject(),cols=askNumber('Columns',3,1,30);if(cols===null)return;const rows=askNumber('Rows',2,1,30);if(rows===null)return;const gap=askNumber('Gap in mm',5,0,100);if(gap===null)return;if(!Number.isInteger(cols)||!Number.isInteger(rows)||cols*rows>200)throw Error('Use whole counts, maximum 200 copies');const[x,y,mx,my]=bounds(o.paths);for(let row=0;row<rows;row++)for(let col=0;col<cols;col++){if(!row&&!col)continue;const copy=structuredClone(o);copy.paths=copy.paths.map(p=>p.map(([px,py])=>[px+col*(mx-x+gap),py+row*(my-y+gap)]));objects.push(copy);}invalidate();draw();});
async function booleanGeometry(action,items){const result=await api('geometry',{action,objects:items});objects=[{name:action+' result',paths:result.paths}];selected=0;invalidate();draw();}
$('unionObjects').onclick=task(()=>booleanGeometry('union',objects));$('subtractObjects').onclick=task(()=>booleanGeometry('difference',objects));
$('offsetObject').onclick=task(async()=>{const o=selectedObject(),d=askNumber('Offset in mm (negative = inset)',1,-100,100);if(d===null)return;const result=await api('geometry',{action:'offset',objects:[o],distance:d});addGenerated('Offset '+o.name,result.paths);});
$('bezierObject').onclick=task(()=>{const[x,y,w,h]=shape();addGenerated('Bézier curve',[CNCOrnaments.bezier?CNCOrnaments.bezier([x,y],[x,y+h],[x+w,y+h],[x+w,y]):Array.from({length:81},(_,i)=>{const t=i/80,u=1-t;return[x+w*(3*u*t*t+t*t*t),y+3*h*t*u];})]);});
let nodeMode=false,nodeDrag=null;
$('editNodes').onclick=task(()=>{const o=selectedObject();if(o.gear)throw Error('Regenerate gears using their parameters');if(o.paths.flat().length>500)throw Error('This object exceeds 500 editable vertices');nodeMode=!nodeMode;draw();});
const nodeSelection=renderSelection;renderSelection=function(){nodeSelection();if(!nodeMode||!objects[selected])return;objects[selected].paths.forEach((p,pi)=>p.forEach(([x,y],vi)=>{if(vi&&vi===p.length-1&&p[0][0]===x&&p[0][1]===y)return;const n=svgNode('circle',{cx:x,cy:Number($('height').value)-y,r:1.2,fill:'#b63170','data-node':`${pi},${vi}`});$('selection').append(n);}));};
$('canvas').addEventListener('pointerdown',e=>{if(!e.target.dataset.node)return;e.stopImmediatePropagation();const[pi,vi]=e.target.dataset.node.split(',').map(Number);nodeDrag={id:e.pointerId,pi,vi};$('canvas').setPointerCapture(e.pointerId);},true);
$('canvas').addEventListener('pointermove',e=>{if(!nodeDrag)return;e.stopImmediatePropagation();const p=objects[selected].paths[nodeDrag.pi],wasClosed=p.length>2&&p[0][0]===p.at(-1)[0]&&p[0][1]===p.at(-1)[1],[x,y]=canvasPoint(e);p[nodeDrag.vi]=[Math.max(0,Math.min(Number($('width').value),x)),Math.max(0,Math.min(Number($('height').value),y))];if(wasClosed&&nodeDrag.vi===0)p[p.length-1]=[...p[0]];delete objects[selected].textOptions;repaintActiveObject();},true);
$('canvas').addEventListener('pointerup',e=>{if(!nodeDrag)return;e.stopImmediatePropagation();nodeDrag=null;invalidate();draw();},true);
$('makeRelief').onclick=task(async()=>{
  const file=$('reliefFile').files[0];
  if(!file) throw Error('Choose a grayscale image or STL');
  if(file.size>8e6) throw Error('File exceeds 8 MB');
  CNCSolidLinks.reliefStage(file);
  // Image maps go to the server's limit (1024 with the compiled extension); STL maps are rasterised in Python.
  const [x,y,width,height]=shape(), resolution=Number($('reliefResolution').value), stl=/\.stl$/i.test(file.name);
  const most=stl?128:(window.studioSession?.relief_map||128);
  if(!Number.isInteger(resolution)||resolution<16||resolution>most) throw Error(`${stl?'STL':'Image'} relief resolution must be 16–${most}`);
  const data={x,y,width,height,depth:Number($('reliefDepth').value),tool_diameter:Number($('tool_diameter').value),
    stepover:Number($('stepover').value),allowance:Number($('reliefAllowance').value),tool_shape:$('reliefTool').value,resolution};
  if(stl) {
    data.content_base64=await CNCSolidLinks.stlContent(file);
  } else {
    const bitmap=await createImageBitmap(file), c=document.createElement('canvas');
    c.width=c.height=resolution;
    const ctx=c.getContext('2d');
    ctx.fillStyle='white';ctx.fillRect(0,0,resolution,resolution);ctx.drawImage(bitmap,0,0,resolution,resolution);
    bitmap.close();
    const pixels=ctx.getImageData(0,0,resolution,resolution).data;
    if(resolution>256) {
      // Large maps travel as the canvas's RGB bytes, top row first; the server applies the same luminance.
      const rgb=new Uint8Array(resolution*resolution*3);
      for(let i=0,j=0;i<pixels.length;i+=4,j+=3){rgb[j]=pixels[i];rgb[j+1]=pixels[i+1];rgb[j+2]=pixels[i+2];}
      let text='';
      for(let i=0;i<rgb.length;i+=0x8000) text+=String.fromCharCode.apply(null,rgb.subarray(i,i+0x8000));
      data.heightmap_rgb=btoa(text);data.heightmap_size=[resolution,resolution];
    } else data.heightmap=Array.from({length:resolution},(_,row)=>Array.from({length:resolution},(_,col)=>{
      const i=((resolution-row-1)*resolution+col)*4;
      return (.2126*pixels[i]+.7152*pixels[i+1]+.0722*pixels[i+2])/255;
    }));
  }
  message('Calculating cutter contact…');
  const result=await api('relief',data);
  const link=await CNCSolidLinks.reliefLink(file,data,result);
  if($('reliefFile').files[0]!==file) throw Error('The relief source changed. Generate the new selection.');
  const [bx,by,mx,my]=bounds(result.paths);
  if(bx<0||by<0||mx>Number($('width').value)||my>Number($('height').value)) throw Error('Relief exceeds the work area.');
  invalidate();
  $('mode').value='router';$('operation').value='line';
  objects.push({name:'Relief · '+file.name,paths:result.paths,depths:result.path_depths,surface:result.surface,...(link?{solidLink:link}:{})});
  selected=objects.length-1;
  invalidate();modeUI();draw();
  message((result.surface?.backend==='cpp'?(result.device==='gpu'?'C++ accelerated relief on the GPU. ':result.device==='gpu+cpu'?'C++ accelerated relief on the GPU and processor together. ':'C++ accelerated relief. '):'Python relief. ')+result.warnings.join(' '));
});
let yaw=.6,pitch=.8,drag3D=null;
window.render3D=()=>{const c=$('preview3d'),ctx=c.getContext('2d');ctx.clearRect(0,0,c.width,c.height);if(!preview)return;const left=preview.bounds[0]-5,bottom=preview.bounds[1]-5,w=preview.bounds[2]-left+5,h=preview.bounds[3]-bottom+5,scale=Math.min(c.width,c.height)*.8/Math.max(w,h,1);function project3(x,y,z){x-=left+w/2;y-=bottom+h/2;const a=x*Math.cos(yaw)-y*Math.sin(yaw),b=x*Math.sin(yaw)+y*Math.cos(yaw);return[c.width/2+a*scale,c.height/2-(b*Math.cos(pitch)-z*Math.sin(pitch))*scale];}if(window.stockPreview){const map=window.stockPreview.heights,n=map.length;for(let j=0;j<n-1;j++)for(let i=0;i<n-1;i++){const z=(map[j][i]+map[j][i+1]+map[j+1][i+1]+map[j+1][i])/4;ctx.fillStyle=`hsl(${38-Math.min(30,Math.abs(z)*5)} 42% ${60-Math.min(35,Math.abs(z)*5)}%)`;ctx.beginPath();for(const[a,b]of[[i,j],[i+1,j],[i+1,j+1],[i,j+1]])ctx.lineTo(...project3(window.stockPreview.x+a*window.stockPreview.width/(n-1),window.stockPreview.y+b*window.stockPreview.height/(n-1),map[b][a]));ctx.closePath();ctx.fill();}}ctx.strokeStyle='#587482';ctx.beginPath();ctx.moveTo(...project3(left,bottom,0));for(const[x,y]of[[left+w,bottom],[left+w,bottom+h],[left,bottom+h],[left,bottom]])ctx.lineTo(...project3(x,y,0));ctx.stroke();for(const s of preview.segments){if(preview.mode==='laser'&&s[5]===0)continue;ctx.strokeStyle=`hsl(${190+Math.min(100,Math.abs(s[4])*20)} 80% 65%)`;ctx.beginPath();ctx.moveTo(...project3(s[0],s[1],s[6]??s[4]));ctx.lineTo(...project3(s[2],s[3],s[4]));ctx.stroke();}};
$('preview3d').onpointerdown=e=>{drag3D=[e.clientX,e.clientY];$('preview3d').setPointerCapture(e.pointerId);};$('preview3d').onpointermove=e=>{if(!drag3D)return;yaw+=(e.clientX-drag3D[0])*.01;pitch=Math.max(.05,Math.min(3.05,pitch+(e.clientY-drag3D[1])*.01));drag3D=[e.clientX,e.clientY];render3D();};$('preview3d').onpointerup=()=>drag3D=null;
$('extrudeText').onclick=task(async()=>{const o=selectedObject(),r=await api('extrude',{paths:o.paths,height:Number($('textDepth').value)});download('artwork-solid.stl',r.stl,'model/stl');message(`Exported solid extrusion · ${r.triangles} triangles`);});
async function artworkRelief(kind){const n=Number($('reliefResolution').value);if(!Number.isInteger(n)||n<16||n>128)throw Error('Artwork relief resolution must be 16–128');const o=selectedObject(),[bx,by,mx,my]=bounds(o.paths),margin=Number($('tool_diameter').value),x=Math.max(0,bx-margin),y=Math.max(0,by-margin);const result=await api('artwork-relief',{paths:o.paths,kind,x,y,width:mx-x+margin,height:my-y+margin,depth:Number($('textDepth').value),tool_diameter:margin,tool_shape:$('reliefTool').value,stepover:Number($('stepover').value),resolution:Number($('reliefResolution').value)});$('mode').value='router';$('operation').value='line';objects.splice(selected,1);addGenerated(kind+' relief · '+o.name,result.paths);objects[selected].depths=result.path_depths;objects[selected].surface=result.surface;invalidate();modeUI();message((result.surface?.backend==='cpp'?'C++ accelerated relief. ':'Python relief. ')+result.warnings.join(' '));}
$('raisedText').onclick=task(()=>artworkRelief('raised'));$('recessedText').onclick=task(()=>artworkRelief('recessed'));

$('simulateStock').onclick=task(async()=>{if(!preview)throw Error('Generate a preview first');if($('mode').value!=='router')throw Error('Stock removal preview is for router jobs');const tool=preview.simulation_tool,diameter=tool?.diameter??Number($('tool_diameter').value),radius=diameter/2,x=Math.max(0,preview.bounds[0]-radius),y=Math.max(0,preview.bounds[1]-radius);window.stockPreview=await api('simulate',{segments:preview.segments,x,y,width:preview.bounds[2]+radius-x,height:preview.bounds[3]+radius-y,tool_diameter:diameter,tool_shape:tool?.shape??($('operation').value==='vcarve'?'v':$('reliefTool').value),v_angle:tool?.angle??Number($('v_angle').value),stock_thickness:Number($('stock_thickness').value),resolution:48});render3D();message(window.stockPreview.warnings.join(' '));});

$('applyHead').onclick=task(async()=>{if(!confirm('Confirm the selected head is mounted, then re-establish Z/focus and work origin. Apply controller mode with outputs off?'))return;const r=await api('head',{mode:$('mode').value,confirmed:true});message(r.message);});
