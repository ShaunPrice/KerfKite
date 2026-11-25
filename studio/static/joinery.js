/* Joinery and layout helpers for parts cut with a round cutter: dog-bone corner reliefs, inlays
   (pocket and plug, straight-sided or V-carved) and nesting of parts on the sheet by their outlines. The geometry is pure and also runs under Node.
   Everything acts on design data, in millimetres with Y up; nothing here reaches a controller. */
((root)=>{
'use strict';
const EPS=1e-9;
const sub=(a,b)=>[a[0]-b[0],a[1]-b[1]],len=v=>Math.hypot(v[0],v[1]);
function ring(path){
 if(!Array.isArray(path)||path.length<4)throw Error('A closed outline needs at least three corners');
 for(const p of path)if(!Array.isArray(p)||p.length!==2||!p.every(Number.isFinite))throw Error('The outline has invalid coordinates');
 const first=path[0],last=path[path.length-1];
 if(Math.abs(first[0]-last[0])>1e-6||Math.abs(first[1]-last[1])>1e-6)throw Error('Dog-bones need closed outlines');
 // Without the closing point and without repeated points, which have no direction.
 const out=[];for(const p of path.slice(0,-1))if(!out.length||len(sub(p,out[out.length-1]))>1e-7)out.push(p);
 if(out.length>1&&len(sub(out[0],out[out.length-1]))<=1e-7)out.pop();
 if(out.length<3)throw Error('A closed outline needs at least three corners');
 return out;
}
const area=points=>points.reduce((sum,p,i)=>{const q=points[(i+1)%points.length];return sum+p[0]*q[1]-q[0]*p[1];},0)/2;

/* A dog-bone at each corner a round cutter cannot reach.
   side 'inside': the cutter works inside the outline (a pocket, slot or inside profile), so its convex corners
   stay round. side 'outside': the cutter goes round a part, so the corners that turn into the part stay round.
   At such a corner a circle of the given radius is added whose edge passes through the corner point, centred on
   the bisector, so that a square-cornered mating part seats fully. radius is at least the cutter's radius. */
function dogbone(path,radius,options={}){
 const side=options.side??'inside',minAngle=options.minAngle??10,maxAngle=options.maxAngle??170,steps=options.steps??12;
 if(side!=='inside'&&side!=='outside')throw Error('Choose whether the cutter is inside or outside the outline');
 if(typeof radius!=='number'||!Number.isFinite(radius)||radius<.05||radius>50)throw Error('Dog-bone radius must be between 0.05 and 50 mm');
 const points=ring(path),n=points.length,ccw=area(points)>0;
 const out=[];let made=0,skipped=0;
 for(let i=0;i<n;i++){
   const p=points[i],a=points[(i+n-1)%n],b=points[(i+1)%n];
   const va=sub(a,p),vb=sub(b,p),la=len(va),lb=len(vb),u1=[va[0]/la,va[1]/la],u2=[vb[0]/lb,vb[1]/lb];
   const turn=(p[0]-a[0])*(b[1]-p[1])-(p[1]-a[1])*(b[0]-p[0]);  // > 0: a left turn
   const convex=ccw?turn>EPS:turn<-EPS;
   const wanted=side==='inside'?convex:(!convex&&Math.abs(turn)>EPS);
   const wedge=Math.acos(Math.max(-1,Math.min(1,u1[0]*u2[0]+u1[1]*u2[1])))*180/Math.PI;  // the angle the cutter sits in
   if(!wanted||wedge<minAngle||wedge>maxAngle){out.push(p);continue;}
   const half=wedge*Math.PI/360,reach=2*radius*Math.cos(half);
   // The relief may take at most half of each edge, so that reliefs at both ends of an edge cannot meet.
   if(reach>la/2+EPS||reach>lb/2+EPS){out.push(p);skipped++;continue;}
   const bis=[u1[0]+u2[0],u1[1]+u2[1]],bl=len(bis),c=[p[0]+bis[0]/bl*radius,p[1]+bis[1]/bl*radius];
   const start=[p[0]+u1[0]*reach,p[1]+u1[1]*reach],end=[p[0]+u2[0]*reach,p[1]+u2[1]*reach];
   const angle=q=>Math.atan2(q[1]-c[1],q[0]-c[0]);
   const arc=(from,to)=>{
     let delta=angle(to)-angle(from);
     while(delta>Math.PI)delta-=2*Math.PI;
     while(delta<-Math.PI)delta+=2*Math.PI;
     const count=Math.max(2,Math.ceil(Math.abs(delta)/(Math.PI/2)*steps)),a0=angle(from),list=[];
     for(let k=1;k<count;k++)list.push([c[0]+radius*Math.cos(a0+delta*k/count),c[1]+radius*Math.sin(a0+delta*k/count)]);
     return list;
   };
   out.push(start,...arc(start,p),p,...arc(p,end),end);made++;
 }
 out.push([...out[0]]);
 return {path:out,corners:made,skipped};
}

/* Places for parts on the sheet: rows from the bottom-left corner, tallest parts first, each part at its own size
   with a gap round it. A part may be turned a quarter turn where that is allowed and helps it fit the row. */
function nest(sizes,width,height,gap=2,options={}){
 const rotate=options.rotate===true;
 if(!Array.isArray(sizes)||!sizes.length||sizes.length>500)throw Error('Choose 1 to 500 parts to nest');
 for(const v of [width,height])if(typeof v!=='number'||!Number.isFinite(v)||v<=0||v>10000)throw Error('Set a valid sheet size first');
 if(typeof gap!=='number'||!Number.isFinite(gap)||gap<0||gap>100)throw Error('The gap between parts must be between 0 and 100 mm');
 const items=sizes.map((s,index)=>{
   if(!s||![s.w,s.h].every(v=>typeof v==='number'&&Number.isFinite(v)&&v>=0))throw Error('A part has no size');
   let w=s.w,h=s.h,turned=false;
   if(rotate&&((w>width&&h<=width)||(h>w&&h<=width&&w<=height)))[w,h,turned]=[h,w,true];  // lie parts down, which packs rows tighter
   if(w>width+EPS||h>height+EPS)throw Error(`Part ${index+1} (${s.w.toFixed(1)} × ${s.h.toFixed(1)} mm) is larger than the sheet`);
   return {index,w,h,turned};
 }).sort((a,b)=>b.h-a.h||b.w-a.w||a.index-b.index);
 const placed=[];let x=0,y=0,row=0;
 for(const item of items){
   if(x>0&&x+item.w>width+EPS){x=0;y+=row+gap;row=0;}
   if(y+item.h>height+EPS)throw Error('The parts do not fit the sheet with this gap; use a larger sheet or nest fewer parts');
   placed.push({index:item.index,x,y,w:item.w,h:item.h,turned:item.turned});
   x+=item.w+gap;row=Math.max(row,item.h);
 }
 const used=placed.reduce((b,p)=>[Math.max(b[0],p.x+p.w),Math.max(b[1],p.y+p.h)],[0,0]);
 return {placements:placed.sort((a,b)=>a.index-b.index),used};
}
/* Nesting by the parts' own outlines. Each part is drawn onto a grid of small cells, every cell it touches being
   taken, with half the gap added all round, so that two parts whose cells do not overlap are at least the gap
   apart. Parts are placed largest first, each at the lowest then leftmost place where it fits, in whichever of the
   allowed turns ends lowest on the sheet. A closed outline is solid: nothing is placed in a part's holes.
   parts: [{paths, fixed}], fixed parts are not turned. turns: 'none', 'quarter' or a step in degrees.
   Returns for each part its angle and the transform [cos, sin, tx, ty]: x' = cos·x − sin·y + tx, y' = sin·x + cos·y + ty. */
function nestShapes(parts,width,height,gap=2,options={}){
 const turns=options.turns??'quarter';
 if(!Array.isArray(parts)||!parts.length||parts.length>200)throw Error('Choose 1 to 200 parts to nest');
 for(const v of [width,height])if(typeof v!=='number'||!Number.isFinite(v)||v<=0||v>10000)throw Error('Set a valid sheet size first');
 if(typeof gap!=='number'||!Number.isFinite(gap)||gap<0||gap>100)throw Error('The gap between parts must be between 0 and 100 mm');
 if(turns!=='none'&&turns!=='quarter'&&!(typeof turns==='number'&&turns>=5&&turns<=180&&Math.abs(360/turns-Math.round(360/turns))<1e-9))throw Error('Turn parts by quarter turns, or by a step from 5 to 180 degrees that divides a full turn');
 const angles=turns==='none'?[0]:turns==='quarter'?[0,90,180,270]:Array.from({length:Math.round(360/turns)},(_,k)=>k*turns);
 const cell=options.cell??Math.max(.25,Math.max(width,height)/400);
 if(!(cell>=.05&&cell<=50))throw Error('The nesting grid must be between 0.05 and 50 mm');
 const grow=Math.ceil(gap/2/cell-1e-9),W=Math.ceil(width/cell-1e-9)+2*grow,H=Math.ceil(height/cell-1e-9)+2*grow,words=(W>>5)+2;
 let count=0;
 for(const [index,part] of parts.entries()){
   if(!part||!Array.isArray(part.paths)||!part.paths.length)throw Error(`Part ${index+1} has no outline`);
   for(const path of part.paths){
     if(!Array.isArray(path)||path.length<2)throw Error(`Part ${index+1} has a path with fewer than two points`);
     for(const p of path)if(!Array.isArray(p)||p.length!==2||!p.every(Number.isFinite))throw Error(`Part ${index+1} has invalid coordinates`);
     if((count+=path.length)>200000)throw Error('Simplify the artwork before nesting it');
   }
 }
 // The cells a part takes when turned by an angle, as rows of bits, with the halo of half the gap.
 function mask(part,angle){
   const a=angle*Math.PI/180,c=angle%90?Math.cos(a):[1,0,-1,0][angle/90],s=angle%90?Math.sin(a):[0,1,0,-1][angle/90];
   const turned=part.paths.map(path=>path.map(([x,y])=>[c*x-s*y,s*x+c*y]));
   let minX=Infinity,minY=Infinity,maxX=-Infinity,maxY=-Infinity;
   for(const path of turned)for(const [x,y] of path){minX=Math.min(minX,x);minY=Math.min(minY,y);maxX=Math.max(maxX,x);maxY=Math.max(maxY,y);}
   const bw=maxX-minX,bh=maxY-minY;
   if(bw>width+EPS||bh>height+EPS)return null;
   const w=Math.max(1,Math.ceil(bw/cell-1e-9))+2*grow,h=Math.max(1,Math.ceil(bh/cell-1e-9))+2*grow,raw=new Uint8Array(w*h);
   const mark=(i,j)=>{i=Math.min(w-grow-1,Math.max(grow,i+grow));j=Math.min(h-grow-1,Math.max(grow,j+grow));raw[j*w+i]=1;};
   for(const path of turned){
     const q=path.map(([x,y])=>[(x-minX)/cell,(y-minY)/cell]);
     // Every cell an edge passes through.
     for(let k=0;k+1<q.length;k++){
       let [x0,y0]=q[k],[x1,y1]=q[k+1];if(x0>x1)[x0,y0,x1,y1]=[x1,y1,x0,y0];
       for(let i=Math.floor(x0);i<=Math.floor(x1);i++){
         const xa=Math.max(x0,i),xb=Math.min(x1,i+1),t=x1-x0>1e-12?[(xa-x0)/(x1-x0),(xb-x0)/(x1-x0)]:[0,1];
         const ya=y0+(y1-y0)*t[0],yb=y0+(y1-y0)*t[1];
         for(let j=Math.floor(Math.min(ya,yb));j<=Math.floor(Math.max(ya,yb));j++)mark(i,j);
       }
     }
     // The inside of a closed outline, by the rows through the cells' centres.
     const n=q.length-1;
     if(n<3||Math.abs(q[0][0]-q[n][0])>1e-6/cell||Math.abs(q[0][1]-q[n][1])>1e-6/cell)continue;
     for(let j=0;j<h-2*grow;j++){
       const y=j+.5,cuts=[];
       for(let k=0;k<n;k++){const [xa,ya]=q[k],[xb,yb]=q[k+1];if((ya>y)!==(yb>y))cuts.push(xa+(y-ya)*(xb-xa)/(yb-ya));}
       cuts.sort((p,r)=>p-r);
       for(let k=0;k+1<cuts.length;k+=2)for(let i=Math.floor(cuts[k]);i<=Math.floor(cuts[k+1]);i++)mark(i,j);
     }
   }
   // Half the gap all round: along the rows, then along the columns.
   const rows=[];let cells=0;
   if(grow){
     const wide=new Uint8Array(w*h);
     for(let j=0;j<h;j++)for(let i=0;i<w;i++)if(raw[j*w+i])for(let d=-grow;d<=grow;d++)wide[j*w+i+d]=1;
     raw.fill(0);
     for(let j=0;j<h;j++)for(let i=0;i<w;i++)if(wide[j*w+i])for(let d=-grow;d<=grow;d++)raw[(j+d)*w+i]=1;
   }
   for(let j=0;j<h;j++){const row=new Uint32Array((w>>5)+1);for(let i=0;i<w;i++)if(raw[j*w+i]){row[i>>5]|=1<<(i&31);cells++;}rows.push(row);}
   return {rows,w,h,bw,bh,minX,minY,c,s,cells,angle};
 }
 const sheet=Array.from({length:H},()=>new Uint32Array(words));
 const clear=(m,ix,iy)=>{
   const shift=ix&31,base=ix>>5;
   for(let j=0;j<m.h;j++){
     const row=m.rows[j],line=sheet[iy+j];
     for(let k=0;k<row.length;k++){
       const bits=row[k];if(!bits)continue;
       if(line[base+k]&(bits<<shift))return false;
       if(shift&&(line[base+k+1]&(bits>>>(32-shift))))return false;
     }
   }
   return true;
 };
 const take=(m,ix,iy)=>{
   const shift=ix&31,base=ix>>5;
   for(let j=0;j<m.h;j++){const row=m.rows[j],line=sheet[iy+j];for(let k=0;k<row.length;k++){line[base+k]|=row[k]<<shift;if(shift)line[base+k+1]|=row[k]>>>(32-shift);}}
 };
 const order=parts.map((part,index)=>{const first=mask(part,0)||angles.slice(1).map(a=>part.fixed?null:mask(part,a)).find(Boolean);return {index,part,size:first?first.cells:Infinity};}).sort((a,b)=>b.size-a.size||a.index-b.index);
 const placements=[];let usedX=0,usedY=0;
 for(const {index,part} of order){
   let best=null,fits=false;
   for(const angle of part.fixed?[0]:angles){
     const m=mask(part,angle);if(!m)continue;fits=true;
     // The part's own outline has to lie on the sheet; its halo may hang over the edge.
     const lastX=Math.floor((width-m.bw)/cell+1e-9),lastY=Math.floor((height-m.bh)/cell+1e-9);
     search:for(let iy=0;iy<=lastY;iy++){
       const top=iy*cell+m.bh;
       if(best&&top>best.top+1e-9)break;
       for(let ix=0;ix<=lastX;ix++)if(clear(m,ix,iy)){
         const right=ix*cell+m.bw;
         if(!best||top<best.top-1e-9||(Math.abs(top-best.top)<=1e-9&&right<best.right-1e-9))best={m,ix,iy,top,right};
         break search;
       }
     }
   }
   if(!fits)throw Error(`Part ${index+1} is larger than the sheet`);
   if(!best)throw Error('The parts do not fit the sheet with this gap; use a larger sheet, a smaller gap or nest fewer parts');
   const {m,ix,iy}=best;take(m,ix,iy);
   const x=ix*cell,y=iy*cell;
   placements.push({index,angle:m.angle,x,y,w:m.bw,h:m.bh,transform:[m.c,m.s,x-m.minX,y-m.minY]});
   usedX=Math.max(usedX,x+m.bw);usedY=Math.max(usedY,y+m.bh);
 }
 return {placements:placements.sort((a,b)=>a.index-b.index),used:[usedX,usedY],cell};
}
/* The sizes of a V inlay, checked. The pocket is carved to its depth from the stock top. The plug's outline is
   carved `start` below the top of its own stock, which is how far the plug goes into the pocket, and `above` deeper
   than that, which is the part left standing out of the work. Under the plug there is a gap for glue of the pocket
   depth less the start.
   V-carving stays one sampling step (`resolution`) short of the wall it carves towards, so the pocket's wall
   stands that much proud and the plug would seat resolution ÷ slope high. (The plug's own wall is exact: the bit's
   point runs along its outline.) The plug's outline is therefore carved that much deeper (plugStart), which lets
   it down to where it was meant to sit. */
function vInlay(angle,vDiameter,depth,start,above,resolution=.2){
 const check=(v,lo,hi,what)=>{if(!(typeof v==='number'&&v>=lo&&v<=hi))throw Error(`${what} must be between ${lo} and ${hi}`);};
 check(angle,10,170,'V-bit angle');check(vDiameter,1,100,'V-bit diameter');check(depth,.5,25,'Pocket depth');check(start,.2,25,'The depth the plug goes in by');check(above,.2,25,'The height left above the surface');check(resolution,.1,1,'V detail sampling');
 if(start>depth-.1+1e-9)throw Error('The plug must go in at least 0.1 mm less than the pocket depth, to leave a gap for glue');
 const slope=Math.tan(angle*Math.PI/360),plugStart=Number((start+resolution/slope).toFixed(3)),plugDepth=Number((plugStart+above).toFixed(3));
 // The bit is as wide as its depth allows: at the stock top it must not be wider than itself.
 const deepest=Math.max(depth,plugDepth),need=2*deepest*slope;
 if(need>vDiameter+1e-9)throw Error(`A ${angle}° V-bit cutting ${deepest.toFixed(2)} mm deep is ${need.toFixed(2)} mm wide at the top: use a wider bit, a smaller angle or shallower depths`);
 if(plugDepth>50)throw Error('The plug would be carved more than 50 mm deep');
 // The flat clearing keeps a sampling step clear of where the sloping walls end, as the carves do.
 return {slope,resolution,plugStart,plugDepth,glue:depth-start,pocketInset:depth*slope+resolution,plugOutset:above*slope+resolution};
}
const core={dogbone,nest,nestShapes,vInlay,area:path=>area(ring(path))};
if(typeof module!=='undefined')module.exports=core;
root.KerfJoinery=core;
if(typeof document==='undefined'||!document.querySelector)return;

const $=id=>document.getElementById(id);
const host=document.getElementById('design-panel-shapes')||document.querySelector('main>aside:not(.operations)');
if(!host)return;
const panel=document.createElement('details');panel.id='joineryPanel';
panel.innerHTML=`<summary>Joinery &amp; nesting</summary>
<h3>Dog-bone corners</h3>
<p class="hint">A round cutter leaves inside corners round, so a square part will not seat. A dog-bone cuts a little past each such corner. Select a closed outline first.</p>
<div class="pair"><label>The cutter runs<select id="dogboneSide"><option value="inside">Inside the outline · pocket or slot</option><option value="outside">Round the outside · a part</option></select></label><label>Relief radius · mm<input id="dogboneRadius" type="number" min=".05" max="50" step=".05"></label></div>
<button id="dogboneApply" type="button">Add dog-bones to selected object</button>
<h3>Inlay</h3>
<p class="hint">Makes a pocket and a matching plug from the selected closed shape. <b>Straight-sided</b>: for an end mill; the plug's corners are rounded to the cutter so that it fits the pocket, and it is made smaller all round by the clearance. <b>V-carved</b>: both are carved with a V-bit, so the sloping walls wedge together and inside corners stay sharp; the plug is mirrored, glued in face down and its back cut away.</p>
<label>Inlay<select id="inlayKind"><option value="straight">Straight-sided · end mill</option><option value="v">V-carved · V-bit and a clearing cutter</option></select></label>
<div class="pair"><label><span id="inlayToolLabel">Cutter diameter · mm</span><input id="inlayTool" type="number" min=".1" max="50" step=".001"></label><label id="inlayClearanceRow">Clearance all round · mm<input id="inlayClearance" type="number" min="0" max="2" step=".01" value=".1"></label></div>
<div class="pair" id="inlayV" hidden><label>V-bit angle · °<input id="inlayAngle" type="number" min="10" max="170" step="1" value="60"></label><label>V-bit diameter · mm<input id="inlayVDiameter" type="number" min="1" max="100" step=".01" value="12.7"></label><label>Pocket depth · mm<input id="inlayDepth" type="number" min=".5" max="25" step=".1" value="4"></label><label>Plug goes in by · mm<input id="inlayStart" type="number" min=".2" max="25" step=".1" value="3"></label><label>Plug left above the surface · mm<input id="inlayAbove" type="number" min=".2" max="25" step=".1" value="1.5"></label></div>
<button id="inlayMake" type="button">Make pocket and plug</button>
<h3>Nest parts on the sheet</h3>
<p class="hint">Packs every object whose output is on towards the bottom-left corner of the work area, by its own outline: a part can sit in the hollow of another. Parts may be turned to fit; choose no turning where the grain or a pattern matters. Nothing is placed inside a part's holes. Undo restores the layout.</p>
<label>Turn parts<select id="nestTurns"><option value="quarter">By quarter turns</option><option value="none">Not at all · keep the grain direction</option><option value="45">In steps of 45°</option><option value="15">In steps of 15°</option></select></label>
<div class="pair"><label>Gap between parts · mm<input id="nestGap" type="number" min="0" max="100" step=".5" value="4"></label><label>Margin at the sheet edge · mm<input id="nestMargin" type="number" min="0" max="100" step=".5" value="2"></label></div>
<button id="nestApply" type="button">Nest parts</button>`;
host.append(panel);
const task=root.task||(fn=>fn);
const toolRadius=()=>Number($('tool_diameter')?.value)/2||1.5875;
const syncTool=()=>{$('dogboneRadius').value=Math.ceil((toolRadius()+.05)*100-1e-9)/100;$('inlayTool').value=Number($('tool_diameter')?.value)||3.175;};
syncTool();$('tool_diameter')?.addEventListener('change',syncTool);root.addEventListener('cnc-project-loaded',syncTool);
const closed=path=>path.length>3&&Math.abs(path[0][0]-path.at(-1)[0])<1e-6&&Math.abs(path[0][1]-path.at(-1)[1])<1e-6;
$('dogboneApply').onclick=task(()=>{
 const o=selectedObject(),radius=Number($('dogboneRadius').value);
 if(radius<toolRadius()-1e-9)throw Error(`The relief radius must be at least the cutter's radius, ${toolRadius().toFixed(3)} mm`);
 let corners=0,skipped=0;
 const paths=o.paths.map(path=>{if(!closed(path))return path;const r=dogbone(path,radius,{side:$('dogboneSide').value});corners+=r.corners;skipped+=r.skipped;return r.path;});
 if(!corners)throw Error(skipped?'The edges at the corners are too short for a relief of this radius':'No corner of this outline needs a dog-bone for a cutter on that side');
 // The outline is now drawn geometry: parameters that would regenerate the plain shape are dropped.
 const {shape:_shape,gear:_gear,...rest}=o;
 objects[selected]={...rest,name:o.name.replace(/ · dog-bones$/,'')+' · dog-bones',paths};
 invalidate();draw();
 message(`${corners} dog-bone corners added${skipped?`; ${skipped} corners have edges too short for one`:''}. Generate a new preview.`);
});
const inlayUI=()=>{const v=$('inlayKind').value==='v';$('inlayV').hidden=!v;$('inlayClearanceRow').hidden=v;$('inlayToolLabel').textContent=v?'Clearing cutter diameter · mm':'Cutter diameter · mm';};
$('inlayKind').onchange=inlayUI;inlayUI();
$('inlayMake').onclick=task(async()=>{
 const o=selectedObject(),tool=Number($('inlayTool').value);
 if(!(tool>=.1&&tool<=50))throw Error('Cutter diameter must be between 0.1 and 50 mm');
 if(!o.paths.every(closed))throw Error('An inlay needs closed outlines');
 const settings=editorSettings(),layer=window.CNCDesignLayers?.activeId?.()||'default',base={designLayerId:layer};
 const b=bounds(o.paths);
 if($('inlayKind').value==='v'){
   const angle=Number($('inlayAngle').value),vDiameter=Number($('inlayVDiameter').value),depth=Number($('inlayDepth').value),start=Number($('inlayStart').value),above=Number($('inlayAbove').value);
   // Fine sampling for both carves: the walls have to meet.
   const resolution=Math.max(.1,Math.min(Number(settings.v_resolution)||.2,.2));
   const size=vInlay(angle,vDiameter,depth,start,above,resolution);
   const offset=async(paths,distance)=>(await api('geometry',{action:'offset',objects:[{paths}],distance}).catch(()=>({paths:[]}))).paths||[];
   // The plug is the shape seen from the back: mirrored left to right, beside the shape, inside a border that
   // bounds the stock cut away round it.
   const margin=size.plugOutset+tool*1.5+1,shift=b[2]-b[0]+2*margin+4;
   const mirrored=o.paths.map(path=>path.map(([x,y])=>[b[0]+b[2]-x+shift,y]).reverse());
   const border=[[b[0]+shift-margin,b[1]-margin],[b[2]+shift+margin,b[1]-margin],[b[2]+shift+margin,b[3]+margin],[b[0]+shift-margin,b[3]+margin],[b[0]+shift-margin,b[1]-margin]];
   if(border[1][0]>Number($('width').value)||border[2][1]>Number($('height').value)||border[0][1]<0)throw Error('There is no room beside the shape for the plug and its border; move the shape or enlarge the work area');
   // What is left flat: the pocket's floor inside the sloping walls, and the plug's surround outside them.
   const floor=await offset(o.paths,-size.pocketInset),floorFits=floor.length&&(await offset(floor,-tool/2)).length;
   const surround=await offset(mirrored,size.plugOutset);
   const taken=new Set(objects.map(x=>x.operationSettings?.tool_id).filter(Number.isInteger));
   const free=()=>{let id=1;while(taken.has(id))id++;taken.add(id);return id;};
   const vTool={tool_diameter:vDiameter,v_angle:angle,v_resolution:resolution,tool_id:free(),tool_name:`V-bit · ${angle}° ${vDiameter} mm`,tool_shape:'v'};
   const flat={tool_diameter:tool,stepover:Math.min(tool/2,Math.max(.1,Number((tool*.4).toFixed(3)))),tool_id:free(),tool_name:`Clearing · ${tool} mm flat`,tool_shape:'flat'};
   const step=Math.min(Number(settings.step)||1,10);
   const carve=(name,paths,total,extra={})=>({...base,name,paths,operationSettings:{...settings,mode:'router',operation:'vcarve',depth:total,step:Math.min(step,total),...vTool,...extra}});
   const clear=(name,paths,total)=>({...base,name,paths,operationSettings:{...settings,mode:'router',operation:'pocket',depth:total,step:Math.min(step,total),...flat}});
   const made=[];
   if(floorFits)made.push(clear(`V inlay pocket · clear · ${o.name}`,floor,depth));
   made.push(carve(`V inlay pocket · carve · ${o.name}`,structuredClone(o.paths),depth));
   made.push(clear(`V inlay plug · clear · ${o.name}`,[border,...surround],size.plugDepth));
   made.push(carve(`V inlay plug · carve · ${o.name}`,[border,...mirrored],size.plugDepth,{v_start_depth:size.plugStart}));
   objects=[...objects,...made];selected=objects.length-1;invalidate();draw();
   message(`V inlay added: a pocket ${depth} mm deep and a mirrored plug that goes in ${start} mm, leaving ${size.glue.toFixed(2)} mm for glue under it and ${above} mm standing out to cut away.${floorFits?'':' The pocket is too narrow for the clearing cutter, so the V-bit cuts all of it.'} The clearing and carving use different cutters: use Manual tool changes for a file for each. The original shape is unchanged: turn its output off if it should not be cut.`);
   return;
 }
 const clearance=Number($('inlayClearance').value);
 if(!(clearance>=0&&clearance<=2))throw Error('Clearance must be between 0 and 2 mm');
 // What the cutter can reach inside the shape, then smaller by the clearance: the plug that fits the pocket.
 const reach=await api('geometry',{action:'offset',objects:[{paths:o.paths}],distance:-tool/2});
 const grown=tool/2-clearance;
 const plug=grown>1e-9?await api('geometry',{action:'offset',objects:[{paths:reach.paths}],distance:grown}):reach;
 const shift=b[2]-b[0]+tool*2+4;
 const moved=plug.paths.map(path=>path.map(([x,y])=>[x+shift,y]));
 const pb=bounds(moved);
 if(pb[2]>Number($('width').value))throw Error('There is no room beside the shape for the plug; move the shape left or widen the work area');
 objects=[...objects,
   {...base,name:`Inlay pocket · ${o.name}`,paths:structuredClone(o.paths),operationSettings:{...settings,mode:'router',operation:'pocket',tool_diameter:tool}},
   {...base,name:`Inlay plug · ${o.name}`,paths:moved,operationSettings:{...settings,mode:'router',operation:'outside',tool_diameter:tool}}];
 selected=objects.length-1;invalidate();draw();
 message('Pocket and plug added with their own operations. Set the pocket depth and a plug depth a little greater, then preview. The original shape is unchanged: turn its output off if it should not be cut.');
});
$('nestApply').onclick=task(()=>{
 const gap=Number($('nestGap').value),margin=Number($('nestMargin').value);
 if(!(margin>=0&&margin<=100))throw Error('The margin must be between 0 and 100 mm');
 const indices=objects.map((o,i)=>o.output===false?-1:i).filter(i=>i>=0);
 if(!indices.length)throw Error('There are no objects with output on to nest');
 const width=Number($('width').value),height=Number($('height').value),turns=$('nestTurns').value;
 // A relief or a path with its own depths keeps its direction: its rows are tied to the surface they follow.
 const parts=indices.map(i=>({paths:objects[i].paths,fixed:!!(objects[i].surface||objects[i].depths||objects[i].powers)}));
 const result=nestShapes(parts,width-2*margin,height-2*margin,gap,{turns:turns==='none'||turns==='quarter'?turns:Number(turns)});
 let next=objects,turned=0;
 for(const p of result.placements){
   const index=indices[p.index],[c,s,tx,ty]=p.transform;
   if(!p.angle){next=window.CNCLayout.apply(next,[{index,dx:tx+margin,dy:ty+margin}]);continue;}
   // A turned part is drawn geometry: the parameters that would regenerate it upright are dropped.
   const {shape:_shape,gear:_gear,rotationPivot:_pivot,...rest}=next[index];
   next=next.slice();next[index]={...rest,paths:rest.paths.map(path=>path.map(([x,y])=>[c*x-s*y+tx+margin,s*x+c*y+ty+margin]))};turned++;
 }
 objects=next;
 invalidate();draw();
 message(`${indices.length} parts nested in ${(result.used[0]+2*margin).toFixed(1)} × ${(result.used[1]+2*margin).toFixed(1)} mm of the sheet${turned?`, ${turned} of them turned`:''}. Generate a new preview.`);
});
})(typeof window!=='undefined'?window:globalThis);
