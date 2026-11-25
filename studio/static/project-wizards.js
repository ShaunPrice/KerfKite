/* Project wizards: standard projects drawn from a few dimensions. A finger-jointed box, a grid of halved
   dividers, and a sign blank with hanging holes. The geometry is pure and also runs under Node. A wizard adds
   ordinary closed outlines to the design; nothing here reaches a controller. Millimetres, Y up. */
((root)=>{
'use strict';
const joinery=root.KerfJoinery||(typeof require!=='undefined'?require('./joinery.js'):null);
function number(value,label,low,high,integer=false){
 const n=typeof value==='string'&&value.trim()!==''?Number(value):value;
 if(typeof n!=='number'||!Number.isFinite(n)||n<low||n>high||(integer&&!Number.isInteger(n)))throw Error(`${label} must be ${integer?'a whole number ':''}between ${low} and ${high}`);
 return n;
}
// An odd number of fingers along an edge, each as near the wanted width as the length allows.
function fingers(length,wanted){const n=Math.max(3,Math.round(length/wanted));return n%2?n:n+1;}

/* One panel: a rectangle w × h whose edges are fingered.
   edges = [bottom, right, top, left], each null (straight) or {count, male}. Along a fingered edge the
   segments alternate; a male edge keeps the even ones (so both its ends) and a female edge keeps the odd ones.
   A segment that is not kept is cut back by the thickness. Where two edges meet, what either removes is
   removed, so every corner block of the box belongs to exactly one panel. fit widens every slot by that much
   (negative tightens it, to allow for a laser's kerf). */
function panel(w,h,thickness,edges,fit=0){
 const corners=[[0,0],[w,0],[w,h],[0,h]],dirs=[[1,0],[0,1],[-1,0],[0,-1]],normals=[[0,1],[-1,0],[0,-1],[1,0]],lengths=[w,h,w,h];
 const removed=(edge,k)=>edge?(edge.male?k%2===1:k%2===0):false;
 const endRemoved=(i,last)=>edges[i]?removed(edges[i],last?edges[i].count-1:0):false;
 const points=[];
 for(let i=0;i<4;i++){
   const edge=edges[i],d=dirs[i],n=normals[i],prev=(i+3)%4,c=corners[i];
   // The corner point, set in along each edge that removes its end segment there.
   const inPrev=endRemoved(prev,true)?thickness:0,inThis=endRemoved(i,false)?thickness:0;
   points.push([c[0]+n[0]*inThis+normals[prev][0]*inPrev,c[1]+n[1]*inThis+normals[prev][1]*inPrev]);
   if(!edge)continue;
   const step=lengths[i]/edge.count;
   for(let k=0;k<edge.count-1;k++){
     const here=removed(edge,k),next=removed(edge,k+1);
     // The boundary moves towards the kept side, so the slot is wider by fit in all.
     const at=step*(k+1)+(here?fit/2:-fit/2);
     for(const cut of [here,next]){const depth=cut?thickness:0;points.push([c[0]+d[0]*at+n[0]*depth,c[1]+d[1]*at+n[1]*depth]);}
   }
 }
 points.push([...points[0]]);
 return points;
}

/* A box with finger joints at every edge. width × depth × height are outside dimensions. The bottom carries
   tabs on all four edges; the front and back carry tabs up their sides; the left and right fit between them.
   With a lid, the top is made like the bottom. */
function box(raw){
 const thickness=number(raw.thickness,'Material thickness',.5,50);
 const width=number(raw.width,'Box width',3*thickness,2000),depth=number(raw.depth,'Box depth',3*thickness,2000),height=number(raw.height,'Box height',3*thickness,2000);
 const finger=number(raw.finger??3*thickness,'Finger width',thickness,500),fit=number(raw.fit??0,'Fit allowance',-1,1),lid=raw.lid===true;
 const nw=fingers(width,finger),nd=fingers(depth,finger),nh=fingers(height,finger);
 for(const [length,count,name] of [[width,nw,'width'],[depth,nd,'depth'],[height,nh,'height']])
   if(length/count<thickness+Math.abs(fit)+.2)throw Error(`The fingers along the ${name} would be narrower than the material is thick; use a wider finger or a larger box`);
 const male=count=>({count,male:true}),female=count=>({count,male:false}),top=count=>lid?female(count):null;
 const parts=[
   {name:'Bottom',size:[width,depth],path:panel(width,depth,thickness,[male(nw),male(nd),male(nw),male(nd)],fit)},
   {name:'Front',size:[width,height],path:panel(width,height,thickness,[female(nw),male(nh),top(nw),male(nh)],fit)},
   {name:'Back',size:[width,height],path:panel(width,height,thickness,[female(nw),male(nh),top(nw),male(nh)],fit)},
   {name:'Left side',size:[depth,height],path:panel(depth,height,thickness,[female(nd),female(nh),top(nd),female(nh)],fit)},
   {name:'Right side',size:[depth,height],path:panel(depth,height,thickness,[female(nd),female(nh),top(nd),female(nh)],fit)},
 ];
 if(lid)parts.push({name:'Lid',size:[width,depth],path:panel(width,depth,thickness,[male(nw),male(nd),male(nw),male(nd)],fit)});
 return {parts,fingers:{width:nw,depth:nd,height:nh},inside:[width-2*thickness,depth-2*thickness,height-(lid?2:1)*thickness]};
}

/* Dividers that slot together at half height: strips along the width with slots from the top, and strips
   along the depth with slots from the bottom. */
function dividers(raw){
 const thickness=number(raw.thickness,'Material thickness',.5,50),height=number(raw.height,'Divider height',2,1000);
 const width=number(raw.width,'Inside width',10,2000),depth=number(raw.depth,'Inside depth',10,2000);
 const columns=number(raw.columns,'Compartments across',1,40,true),rows=number(raw.rows,'Compartments deep',1,40,true),fit=number(raw.fit??0,'Fit allowance',-1,1);
 if(columns<2&&rows<2)throw Error('One compartment needs no dividers');
 const slot=thickness+fit;
 const strip=(length,count,fromTop)=>{
   // count slots at equal spacing; each is slot wide and half the height deep.
   const points=fromTop?[[0,0],[length,0],[length,height]]:[[0,height],[0,0]];
   const pitch=length/(count+1);
   if(count&&pitch<slot+1)throw Error('The compartments are too small for this material');
   const xs=Array.from({length:count},(_,i)=>pitch*(i+1));
   if(fromTop){for(const x of xs.reverse())points.push([x+slot/2,height],[x+slot/2,height/2],[x-slot/2,height/2],[x-slot/2,height]);points.push([0,height]);}
   else{for(const x of xs)points.push([x-slot/2,0],[x-slot/2,height/2],[x+slot/2,height/2],[x+slot/2,0]);points.push([length,0],[length,height]);}
   points.push([...points[0]]);return points;
 };
 const parts=[];
 for(let i=0;i<rows-1;i++)parts.push({name:`Divider across ${i+1}`,size:[width,height],path:strip(width,columns-1,true)});
 for(let i=0;i<columns-1;i++)parts.push({name:`Divider deep ${i+1}`,size:[depth,height],path:strip(depth,rows-1,false)});
 return {parts,compartment:[(width-(columns-1)*thickness)/columns,(depth-(rows-1)*thickness)/rows]};
}

/* A sign blank: a rectangle with rounded corners and, optionally, two hanging holes near the top. */
function sign(raw){
 const width=number(raw.width,'Sign width',10,2000),height=number(raw.height,'Sign height',10,2000);
 const radius=number(raw.radius??0,'Corner radius',0,Math.min(width,height)/2),hole=number(raw.hole??0,'Hole diameter',0,50);
 const inset=number(raw.inset??Math.max(10,hole*2),'Hole inset',hole/2+1,Math.min(width/2,height)-hole/2-1+1e-9);
 const outline=[],steps=16;
 const corner=(cx,cy,start)=>{if(radius<=0){outline.push([cx,cy]);return;}for(let k=0;k<=steps;k++){const a=start+Math.PI/2*k/steps;outline.push([cx+radius*Math.cos(a),cy+radius*Math.sin(a)]);}};
 if(radius>0){corner(width-radius,radius,-Math.PI/2);corner(width-radius,height-radius,0);corner(radius,height-radius,Math.PI/2);corner(radius,radius,Math.PI);}
 else outline.push([0,0],[width,0],[width,height],[0,height]);
 outline.push([...outline[0]]);
 const holes=[];
 if(hole>0)for(const x of [inset,width-inset])holes.push(Array.from({length:49},(_,i)=>[x+hole/2*Math.cos(i*Math.PI/24),height-inset+hole/2*Math.sin(i*Math.PI/24)]));
 return {outline,holes};
}
const api={panel,box,dividers,sign,fingers};
if(typeof module!=='undefined')module.exports=api;
root.KerfProjectWizards=api;
if(typeof document==='undefined'||!document.querySelector)return;

const $=id=>document.getElementById(id);
const host=document.getElementById('design-panel-templates')||document.getElementById('design-panel-shapes')||document.querySelector('main>aside:not(.operations)');
if(!host)return;
const section=document.createElement('details');section.id='projectWizards';
const field=(id,label,value,attrs='')=>`<label>${label}<input id="${id}" type="number" value="${value}" ${attrs}></label>`;
section.innerHTML=`<summary>Project wizards</summary>
<p class="hint">Draws a standard project from its dimensions as ordinary outlines, laid out on the sheet. Check the fit on scrap of the same material before cutting the whole project.</p>
<label>Project<select id="wizardKind"><option value="box">Finger-jointed box</option><option value="dividers">Slot-together dividers</option><option value="sign">Sign blank with hanging holes</option></select></label>
<div id="wizardBox"><div class="pair">${field('boxWidth','Outside width · mm',120,'min="1" step="1"')}${field('boxDepth','Outside depth · mm',80,'min="1" step="1"')}${field('boxHeight','Outside height · mm',60,'min="1" step="1"')}${field('boxThickness','Material thickness · mm',6,'min=".5" max="50" step=".1"')}${field('boxFinger','Finger width · mm',18,'min="1" step="1"')}${field('boxFit','Fit allowance · mm',0,'min="-1" max="1" step=".01"')}</div><label class="check"><input type="checkbox" id="boxLid"> With a lid, jointed like the bottom</label><label class="check"><input type="checkbox" id="boxDogbones" checked> Dog-bone the inside corners for a router (uses the CAM cutter diameter)</label></div>
<div id="wizardDividers" hidden><div class="pair">${field('dividerWidth','Inside width · mm',200,'min="10" step="1"')}${field('dividerDepth','Inside depth · mm',150,'min="10" step="1"')}${field('dividerHeight','Divider height · mm',40,'min="2" step="1"')}${field('dividerThickness','Material thickness · mm',3,'min=".5" max="50" step=".1"')}${field('dividerColumns','Compartments across',3,'min="1" max="40" step="1"')}${field('dividerRows','Compartments deep',2,'min="1" max="40" step="1"')}${field('dividerFit','Fit allowance · mm',0,'min="-1" max="1" step=".01"')}</div></div>
<div id="wizardSign" hidden><div class="pair">${field('signWidth','Width · mm',200,'min="10" step="1"')}${field('signHeight','Height · mm',100,'min="10" step="1"')}${field('signRadius','Corner radius · mm',10,'min="0" step="1"')}${field('signHole','Hanging hole diameter · mm',5,'min="0" max="50" step=".5"')}${field('signInset','Hole inset · mm',12,'min="1" step="1"')}</div></div>
${field('wizardGap','Gap between parts · mm',6,'min="0" max="100" step=".5"')}
<button id="wizardAdd" type="button" class="primary">Add project to the design</button>
<p id="wizardStatus" class="hint" role="status"></p>`;
host.append(section);
const task=root.task||(fn=>fn);
$('wizardKind').onchange=()=>{for(const [id,kind] of [['wizardBox','box'],['wizardDividers','dividers'],['wizardSign','sign']])$(id).hidden=$('wizardKind').value!==kind;};
const value=id=>$(id).value;
$('wizardAdd').onclick=task(()=>{
 const kind=$('wizardKind').value,gap=number(value('wizardGap'),'Gap between parts',0,100),margin=2;
 const width=Number($('width').value),height=Number($('height').value),tool=Number($('tool_diameter').value)||3.175;
 let parts,note;
 if(kind==='box'){
   const made=box({width:value('boxWidth'),depth:value('boxDepth'),height:value('boxHeight'),thickness:value('boxThickness'),finger:value('boxFinger'),fit:value('boxFit'),lid:$('boxLid').checked});
   parts=made.parts.map(p=>({name:`Box · ${p.name}`,paths:[p.path],operation:'outside'}));
   let relieved=0,short=0;
   if($('boxDogbones').checked)for(const part of parts){const r=joinery.dogbone(part.paths[0],tool/2+.05,{side:'outside'});relieved+=r.corners;short+=r.skipped;part.paths=[r.path];}
   note=`Box added: ${parts.length} panels, ${made.fingers.width} × ${made.fingers.depth} × ${made.fingers.height} fingers, inside ${made.inside.map(v=>v.toFixed(1)).join(' × ')} mm${relieved?`, ${relieved} dog-bones`:''}.${short?` ${short} corners are too tight for a dog-bone with this cutter: use a smaller cutter or wider fingers.`:''}`;
 }else if(kind==='dividers'){
   const made=dividers({width:value('dividerWidth'),depth:value('dividerDepth'),height:value('dividerHeight'),thickness:value('dividerThickness'),columns:value('dividerColumns'),rows:value('dividerRows'),fit:value('dividerFit')});
   parts=made.parts.map(p=>({name:p.name,paths:[p.path],operation:'outside'}));
   note=`${parts.length} dividers added; each compartment is ${made.compartment.map(v=>v.toFixed(1)).join(' × ')} mm.`;
 }else{
   const made=sign({width:value('signWidth'),height:value('signHeight'),radius:value('signRadius'),hole:value('signHole'),inset:value('signInset')});
   parts=[{name:'Sign blank',paths:[made.outline],operation:'outside'}];
   if(made.holes.length)parts.push({name:'Sign hanging holes',paths:made.holes,operation:'inside',keep:true});
   note='Sign blank added. Cut the holes before the outline.';
 }
 // Lay the parts out in rows; the holes of a sign stay on their blank.
 const loose=parts.filter(p=>!p.keep),boxes=loose.map(p=>window.CNCLayout.extent(p));
 const placed=joinery.nest(boxes.map(b=>({w:b[2]-b[0],h:b[3]-b[1]})),width-2*margin,height-2*margin,gap);
 const shift=new Map(placed.placements.map(p=>[loose[p.index],[margin+p.x-boxes[p.index][0],margin+p.y-boxes[p.index][1]]]));
 const sheetShift=shift.get(loose[0]);
 const settings=editorSettings(),router=settings.mode==='router',layer=window.CNCDesignLayers?.activeId?.()||'default';
 const added=parts.map(p=>{
   const [dx,dy]=shift.get(p)||sheetShift;
   const object={designLayerId:layer,name:p.name,paths:p.paths.map(path=>path.map(([x,y])=>[x+dx,y+dy]))};
   // A router follows the outline at the cutter's radius; a laser or knife follows the line itself.
   if(router)object.operationSettings={...settings,operation:p.operation};
   return object;
 });
 // Holes are cut before the outline they sit in.
 added.sort((a,b)=>(a.name==='Sign hanging holes'?0:1)-(b.name==='Sign hanging holes'?0:1));
 objects=[...objects,...added];selected=objects.length-1;invalidate();draw();
 $('wizardStatus').textContent=note;
 message(note+(router?' Each part has an outside profile; set the depth to the material thickness and add holding tabs.':' Generate a preview.'));
});
})(typeof window!=='undefined'?window:globalThis);
