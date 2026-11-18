/* Object-relative pivots follow translations/resizing and survive project saves. */
(function(root){'use strict';
function box(paths){let x=Infinity,y=Infinity,mx=-Infinity,my=-Infinity,count=0;
 if(!Array.isArray(paths)||!paths.length)throw Error('Select vector geometry to rotate.');
 for(const path of paths){if(!Array.isArray(path))throw Error('Invalid geometry');for(const p of path){if(++count>100000||!Array.isArray(p)||p.length<2||!p.slice(0,2).every(Number.isFinite))throw Error('Invalid or oversized rotation geometry');x=Math.min(x,p[0]);y=Math.min(y,p[1]);mx=Math.max(mx,p[0]);my=Math.max(my,p[1]);}}
 if(!count)throw Error('Empty rotation geometry');return [x,y,mx,my];}
function pivot(o){const[x,y,mx,my]=box(o.paths),p=o.rotationPivot;
 return p&&[p.u,p.v].every(Number.isFinite)?[x+p.u*(mx-x||1),y+p.v*(my-y||1)]:[(x+mx)/2,(y+my)/2];}
function relative(paths,p){if(!Array.isArray(p)||p.length!==2||!p.every(Number.isFinite))throw Error('Invalid pivot');const[x,y,mx,my]=box(paths);return {u:(p[0]-x)/(mx-x||1),v:(p[1]-y)/(my-y||1)};}
function rotate(o,angle,width,height){if(o.surface)throw Error('Surface relief cannot be rotated here; regenerate its surface and toolpaths in the required orientation.');
 if(![angle,width,height].every(Number.isFinite)||width<=0||height<=0||Math.abs(angle)>Math.PI*10000)throw Error('Invalid rotation or work area');
 const p=pivot(o),c=Math.cos(angle),s=Math.sin(angle),paths=o.paths.map(path=>path.map(([x,y])=>[p[0]+(x-p[0])*c-(y-p[1])*s,p[1]+(x-p[0])*s+(y-p[1])*c]));
 if(paths.some(path=>path.some(([x,y])=>!Number.isFinite(x)||!Number.isFinite(y)||x< -1e-8||y< -1e-8||x>width+1e-8||y>height+1e-8)))throw Error('Rotation exceeds the work area. Move the object inward or change the pivot.');
 for(const path of paths)for(const p of path){p[0]=Math.max(0,Math.min(width,p[0]));p[1]=Math.max(0,Math.min(height,p[1]));}
 return {...o,paths,rotationPivot:relative(paths,p)};
}
function delta(start,end,p,snap=false){let a=Math.atan2(end[1]-p[1],end[0]-p[0])-Math.atan2(start[1]-p[1],start[0]-p[0]);a=Math.atan2(Math.sin(a),Math.cos(a));return snap?Math.round(a/(Math.PI/12))*Math.PI/12:a;}
const api={box,pivot,relative,rotate,delta};root.CNCRotation=api;if(typeof module!=='undefined'&&module.exports)module.exports=api;if(typeof document==='undefined')return;
let drag=null;
api.rotateSelected=function(degrees){if(drag)throw Error('Finish the current drag first.');if(!Number.isFinite(degrees))throw Error('Rotation must be finite degrees.');const o=objects[selected];if(!o)throw Error('Select an object first.');const result=rotate(o,degrees*Math.PI/180,Number($('width').value),Number($('height').value));invalidate();o.paths=result.paths;o.rotationPivot=result.rotationPivot;draw();window.recordHistory?.();};
api.setSelectedPivot=function(x,y){if(drag)throw Error('Finish the current drag first.');const o=objects[selected],w=Number($('width').value),h=Number($('height').value);if(!o)throw Error('Select an object first.');if(![x,y].every(Number.isFinite)||x<0||y<0||x>w||y>h)throw Error('Pivot must be inside the work area.');const p=relative(o.paths,[x,y]);invalidate();o.rotationPivot=p;draw();window.recordHistory?.();};
api.resetPivot=function(){if(drag)throw Error('Finish the current drag first.');const o=objects[selected];if(!o)throw Error('Select an object first.');invalidate();delete o.rotationPivot;draw();window.recordHistory?.();};
const previous=renderSelection;
renderSelection=function(){previous();const o=objects[selected];if(!o||o.surface)return;let p,b;try{p=pivot(o);b=box(o.paths);}catch{return;}
 const h=Number($('height').value),m=$('canvas').getScreenCTM(),unit=m?1/Math.max(.001,Math.hypot(m.a,m.b)):1,r=7*unit;
 const handle=[(b[0]+b[2])/2,Math.min(h+8,b[3]+25*unit)],layer=$('selection');
 const line=svgNode('line',{x1:p[0],y1:h-p[1],x2:handle[0],y2:h-handle[1],stroke:'#a95c10','stroke-width':unit,'pointer-events':'none','stroke-dasharray':`${4*unit} ${3*unit}`});layer.append(line);
 for(const[k,pt,color]of [['rotate',handle,'#c77829'],['pivot',p,'#176a9a']]){const n=svgNode('circle',{cx:pt[0],cy:h-pt[1],r,fill:'white',stroke:color,'stroke-width':2*unit,'data-rotation':k,'pointer-events':'all',role:'img','aria-label':k==='pivot'?'Drag rotation pivot':'Drag to rotate; Shift snaps to 15 degrees'});n.style.cursor=k==='pivot'?'crosshair':'grab';const title=svgNode('title',{});title.textContent=k==='pivot'?'Drag the rotation pivot. Double-click to reset to centre.':'Drag to rotate freely. Hold Shift for 15° steps. Escape cancels.';n.append(title);layer.append(n);if(k==='pivot'){for(const[a,b,c,d]of [[pt[0]-r*.6,h-pt[1],pt[0]+r*.6,h-pt[1]],[pt[0],h-pt[1]-r*.6,pt[0],h-pt[1]+r*.6]])layer.append(svgNode('line',{x1:a,y1:b,x2:c,y2:d,stroke:color,'stroke-width':unit,'pointer-events':'none'}));}}
};
const canvas=$('canvas');
function swallow(e){e.stopImmediatePropagation();e.preventDefault();}
canvas.addEventListener('pointerdown',e=>{const kind=e.target.dataset.rotation;if(!kind||e.button!==0)return;swallow(e);if(drag)return;
 const o=objects[selected];if(!o||o.surface)return;try{const start=canvasPoint(e);window.recordHistory?.();drag={kind,id:e.pointerId,index:selected,object:o,original:structuredClone(o),start,p:pivot(o),changed:false};canvas.setPointerCapture(e.pointerId);message(kind==='pivot'?'Drag pivot; double-click resets it to centre.':'Drag to rotate; Shift snaps to 15°. Escape cancels.');}catch(err){drag=null;message(err.message,true);}},true);
canvas.addEventListener('pointermove',e=>{if(!drag||drag.id!==e.pointerId)return;swallow(e);const g=drag;if(objects[g.index]!==g.object){finish(true);return;}
 try{const p=canvasPoint(e),w=Number($('width').value),h=Number($('height').value);let result;
 if(g.kind==='pivot'){const q=[Math.max(0,Math.min(w,p[0])),Math.max(0,Math.min(h,p[1]))];result={...g.original,rotationPivot:relative(g.original.paths,q)};}
 else {if(Math.hypot(p[0]-g.p[0],p[1]-g.p[1])<1e-6)return;result=rotate(g.original,delta(g.start,p,g.p,e.shiftKey),w,h);}
 if(!g.changed){invalidate();g.changed=true;}g.object.paths=result.paths;g.object.rotationPivot=result.rotationPivot;draw();
 }catch(err){message(err.message,true);}},true);
function finish(cancel=false){if(!drag)return;const g=drag;drag=null;
 if(cancel&&objects[g.index]===g.object){g.object.paths=g.original.paths;if(g.original.rotationPivot===undefined)delete g.object.rotationPivot;else g.object.rotationPivot=g.original.rotationPivot;if(g.changed)invalidate();}
 if(canvas.hasPointerCapture(g.id))canvas.releasePointerCapture(g.id);draw();if(!cancel&&g.changed){window.recordHistory?.();message(g.kind==='pivot'?'Rotation pivot saved.':'Rotation applied. Generate a new toolpath preview.');}}
canvas.addEventListener('pointerup',e=>{if(drag&&drag.id===e.pointerId){swallow(e);finish();}},true);
canvas.addEventListener('pointercancel',e=>{if(drag&&drag.id===e.pointerId){swallow(e);finish(true);}},true);
canvas.addEventListener('lostpointercapture',()=>finish(true));
canvas.addEventListener('dblclick',e=>{if(e.target.dataset.rotation!=='pivot')return;swallow(e);const o=objects[selected];if(!o)return;invalidate();delete o.rotationPivot;draw();window.recordHistory?.();});
window.addEventListener('keydown',e=>{if(!drag)return;if(e.key==='Escape'){swallow(e);finish(true);}else if((e.ctrlKey||e.metaKey)&&['z','y'].includes(e.key.toLowerCase())){swallow(e);finish(true);}},true);
window.addEventListener('blur',()=>finish(true));renderSelection();
})(typeof globalThis!=='undefined'?globalThis:this);
