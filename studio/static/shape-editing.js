/* Parameter editing regenerates geometry in its original frame, preserving later transforms. */
(function(root){'use strict';
const shapes=root.CNCShapes||(typeof require==='function'?require('./shapes.js'):null);
function affine(source,target){
 if(source.length!==target.length||source.length<4)throw Error('Shape vertices were edited; add a new parametric shape to change these settings.');
 const n=source.length-1,ids=[0,Math.floor(n/3),Math.floor(2*n/3)],p=ids.map(i=>source[i]),q=ids.map(i=>target[i]);
 const ux=p[1][0]-p[0][0],uy=p[1][1]-p[0][1],vx=p[2][0]-p[0][0],vy=p[2][1]-p[0][1],det=ux*vy-uy*vx;
 if(Math.abs(det)<1e-10)throw Error('Shape is too narrow to edit parametrically.');
 const forward=([x,y])=>{const a=((x-p[0][0])*vy-(y-p[0][1])*vx)/det,b=(ux*(y-p[0][1])-uy*(x-p[0][0]))/det;return [q[0][0]+a*(q[1][0]-q[0][0])+b*(q[2][0]-q[0][0]),q[0][1]+a*(q[1][1]-q[0][1])+b*(q[2][1]-q[0][1])];};
 for(let i=0;i<source.length;i++){const v=forward(source[i]);if(Math.hypot(v[0]-target[i][0],v[1]-target[i][1])>1e-5)throw Error('Shape vertices were edited; add a new parametric shape to change these settings.');}
 return forward;
}
function definition(o){
 if(o.gear)return {kind:'gear',options:{...o.gear}};
 if(o.polygon)return {kind:'polygon',options:{...o.polygon}};
 // Older saved polygons had only a generated name and contours; recover their local frame.
 const match=/^(\d+)-sided polygon$/.exec(o.name||'');if(!match)return null;
 const sides=Number(match[1]),outer=o.paths.at(-1);if(outer.length!==sides+1)return null;
 const unit={x:0,y:0,width:100,height:100,sides,rotation:0,bore:0},map=affine(shapes.polygon(unit)[0],outer),origin=map([0,0]),x=map([100,0]),y=map([0,100]);
 const options={...unit,width:Math.hypot(x[0]-origin[0],x[1]-origin[1]),height:Math.hypot(y[0]-origin[0],y[1]-origin[1])};
 if(o.paths.length===2){const center=map([50,50]);options.bore=2*Math.hypot(o.paths[0][0][0]-center[0],o.paths[0][0][1]-center[1]);}
 if(o.paths.length>2)return null;
 return {kind:'polygon',options};
}
function regenerate(o,changes,workWidth,workHeight){
 const d=definition(o);if(!d)throw Error('Select a gear or polygon first.');
 if(o.depths||o.powers||o.surface)throw Error('This shape has depth or power data; edit a fresh parametric shape to preserve machining data.');
 const old=shapes[d.kind](d.options),map=affine(old.at(-1),o.paths.at(-1));
 if(old.length!==o.paths.length)throw Error('Shape contours were edited; add a new parametric shape.');
 for(let i=0;i<old.length-1;i++){if(old[i].length!==o.paths[i].length)throw Error('Shape contours were edited; add a new parametric shape.');for(let j=0;j<old[i].length;j++){const p=map(old[i][j]);if(Math.hypot(p[0]-o.paths[i][j][0],p[1]-o.paths[i][j][1])>1e-5)throw Error('Shape contours were edited; add a new parametric shape.');}}
 const options={...d.options,...changes};
 if(d.kind==='gear'){const before=shapes.gearDimensions(d.options).outside,after=shapes.gearDimensions(options).outside;options.x=Number(d.options.x)+(before-after)/2;options.y=Number(d.options.y)+(before-after)/2;}
 // Generation needs nonnegative local coordinates even when enlargement crosses its old origin.
 let shiftX=Math.max(0,-Number(options.x)),shiftY=Math.max(0,-Number(options.y));
 const paths=shapes[d.kind]({...options,x:Number(options.x)+shiftX,y:Number(options.y)+shiftY}).map(p=>p.map(([x,y])=>map([x-shiftX,y-shiftY])));
 if(paths.some(p=>p.some(([x,y])=>!Number.isFinite(x)||!Number.isFinite(y)||x<0||y<0||x>workWidth||y>workHeight)))throw Error('Updated shape exceeds the work area; reduce its size or move it inward.');
 // Keep source coordinates valid for the next regeneration and store the matching frame.
 if(shiftX||shiftY){options.x=Number(options.x)+shiftX;options.y=Number(options.y)+shiftY;}
 return {...o,paths,[d.kind]:options,name:/^(Involute gear · \d+ teeth|\d+-sided polygon)$/.test(o.name)?(d.kind==='gear'?`Involute gear · ${options.teeth} teeth`:`${options.sides}-sided polygon`):o.name};
}
const api={definition,regenerate};root.CNCShapeEditing=api;if(typeof module!=='undefined'&&module.exports)module.exports=api;
if(typeof document==='undefined')return;
const gearFields={gearTeeth:'teeth',gearModule:'module',gearPressure:'pressureAngle',gearBacklash:'backlash',gearBore:'bore'},polygonFields={polygonSides:'sides',polygonBore:'bore'};
let syncedObject=null,syncedDefinition='';
function sync(){const o=objects[selected];let d;try{d=o&&definition(o);}catch{return;}const key=JSON.stringify(d);if(o===syncedObject&&key===syncedDefinition)return;syncedObject=o;syncedDefinition=key;if(!d)return;for(const[id,key]of Object.entries(d.kind==='gear'?gearFields:polygonFields))$(id).value=d.options[key]??0;$('shapeRotation').value=d.options.rotation??0;if(d.kind==='gear')gearSize();}
const previousDraw=draw;draw=function(){previousDraw();sync();};
function update(kind,property,input){const o=objects[selected];let d;try{d=o&&definition(o);}catch(e){message(e.message,true);return;}if(!d||kind&&d.kind!==kind)return;
 invalidate();
 try{if(!input.value.trim())throw Error('Enter a value to update the selected shape.');const replacement=regenerate(o,{[property]:Number(input.value)},Number($('width').value),Number($('height').value));objects[selected]=replacement;draw();window.recordHistory?.();message('Selected shape updated. Generate a new toolpath preview before exporting or running.');}
 catch(e){message(e.message,true);}
}
for(const[kind,fields]of [['gear',gearFields],['polygon',polygonFields]])for(const[id,key]of Object.entries(fields))$(id).addEventListener('input',()=>update(kind,key,$(id)));
$('shapeRotation').addEventListener('input',()=>update(null,'rotation',$('shapeRotation')));
sync();
})(typeof globalThis!=='undefined'?globalThis:this);
