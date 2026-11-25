/* Joinery and layout helpers for parts cut with a round cutter: dog-bone corner reliefs, a plain inlay
   (pocket and plug) and nesting of parts on the sheet. The geometry is pure and also runs under Node.
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
const core={dogbone,nest,area:path=>area(ring(path))};
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
<p class="hint">Makes a pocket and a matching plug from the selected closed shape, for straight-sided cutters. The plug's corners are rounded to the cutter so that it fits the pocket, and it is made smaller all round by the clearance.</p>
<div class="pair"><label>Cutter diameter · mm<input id="inlayTool" type="number" min=".1" max="50" step=".001"></label><label>Clearance all round · mm<input id="inlayClearance" type="number" min="0" max="2" step=".01" value=".1"></label></div>
<button id="inlayMake" type="button">Make pocket and plug</button>
<h3>Nest parts on the sheet</h3>
<p class="hint">Moves every object whose output is on into rows from the bottom-left corner of the work area, by its bounding box. Parts are not turned. Undo restores the layout.</p>
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
$('inlayMake').onclick=task(async()=>{
 const o=selectedObject(),tool=Number($('inlayTool').value),clearance=Number($('inlayClearance').value);
 if(!(tool>=.1&&tool<=50))throw Error('Cutter diameter must be between 0.1 and 50 mm');
 if(!(clearance>=0&&clearance<=2))throw Error('Clearance must be between 0 and 2 mm');
 if(!o.paths.every(closed))throw Error('An inlay needs closed outlines');
 // What the cutter can reach inside the shape, then smaller by the clearance: the plug that fits the pocket.
 const reach=await api('geometry',{action:'offset',objects:[{paths:o.paths}],distance:-tool/2});
 const grown=tool/2-clearance;
 const plug=grown>1e-9?await api('geometry',{action:'offset',objects:[{paths:reach.paths}],distance:grown}):reach;
 const b=bounds(o.paths),shift=b[2]-b[0]+tool*2+4;
 const moved=plug.paths.map(path=>path.map(([x,y])=>[x+shift,y]));
 const pb=bounds(moved);
 if(pb[2]>Number($('width').value))throw Error('There is no room beside the shape for the plug; move the shape left or widen the work area');
 const settings=editorSettings(),layer=window.CNCDesignLayers?.activeId?.()||'default';
 const base={designLayerId:layer};
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
 const boxes=indices.map(i=>window.CNCLayout.extent(objects[i]));
 const width=Number($('width').value),height=Number($('height').value);
 const result=nest(boxes.map(b=>({w:b[2]-b[0],h:b[3]-b[1]})),width-2*margin,height-2*margin,gap);
 const moves=result.placements.map(p=>({index:indices[p.index],dx:margin+p.x-boxes[p.index][0],dy:margin+p.y-boxes[p.index][1]}));
 objects=window.CNCLayout.apply(objects,moves);
 invalidate();draw();
 message(`${indices.length} parts nested in ${(result.used[0]+2*margin).toFixed(1)} × ${(result.used[1]+2*margin).toFixed(1)} mm of the sheet. Generate a new preview.`);
});
})(typeof window!=='undefined'?window:globalThis);
