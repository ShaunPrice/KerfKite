/* Metric, zero-profile-shift external spur gears. All dimensions in mm. */
(function(root){
'use strict';
function numeric(value,name,min,max,integer=false){const n=Number(value);if(!Number.isFinite(n)||n<min||n>max||(integer&&!Number.isInteger(n)))throw Error(`${name} must be ${integer?'a whole number ':''}between ${min} and ${max}`);return n;}
function closed(points){return [...points,[...points[0]]];}
function hole(cx,cy,bore){const n=Math.max(64,Math.ceil(Math.PI*bore/.1));return closed(Array.from({length:n},(_,i)=>{const angle=-2*Math.PI*i/n;return[cx+bore/2*Math.cos(angle),cy+bore/2*Math.sin(angle)];}));}
function polygon({x,y,width,height,sides,rotation=0,bore=0}){x=numeric(x,'X',0,2000);y=numeric(y,'Y',0,2000);width=numeric(width,'Width',.1,2000);height=numeric(height,'Height',.1,2000);sides=numeric(sides,'Sides',3,128,true);rotation=numeric(rotation,'Rotation',-360,360);bore=numeric(bore,'Centre hole diameter',0,2000);const angle=rotation*Math.PI/180,cx=x+width/2,cy=y+height/2;const p=closed(Array.from({length:sides},(_,i)=>{const a=angle+2*Math.PI*i/sides;return[cx+width/2*Math.cos(a),cy+height/2*Math.sin(a)];}));const clearance=Math.min(...p.slice(0,-1).map(([ax,ay],i)=>{const[bx,by]=p[i+1];return Math.abs((bx-ax)*(cy-ay)-(by-ay)*(cx-ax))/Math.hypot(bx-ax,by-ay);}));if(bore>0&&bore/2>=clearance)throw Error('Centre hole must fit inside the polygon');return bore>0?[hole(cx,cy,bore),p]:[p];}
function gearDimensions({teeth,module:mod,pressureAngle=20,backlash=0,bore=0}){
 teeth=numeric(teeth,'Teeth',6,200,true);mod=numeric(mod,'Module',.1,50);pressureAngle=numeric(pressureAngle,'Pressure angle',14.5,25);backlash=numeric(backlash,'Tooth thinning at pitch circle',0,10);bore=numeric(bore,'Bore diameter',0,2000);
 const alpha=pressureAngle*Math.PI/180,minTeeth=Math.ceil(2/Math.sin(alpha)**2);
 if(teeth<minTeeth)throw Error(`Use at least ${minTeeth} teeth at ${pressureAngle}°; smaller pinions require profile-shift/undercut design`);
 const pitch=mod*teeth, outside=mod*(teeth+2),rootDiameter=mod*(teeth-2.5),base=pitch*Math.cos(alpha);
 if(outside>2000)throw Error('Gear outside diameter exceeds 2000 mm');
 if(bore>=rootDiameter)throw Error('Bore must be smaller than the root diameter');
 if(backlash>=Math.PI*mod/2)throw Error('Tooth thinning would remove the teeth');
 return {teeth,module:mod,pressureAngle,backlash,bore,pitch,outside,root:rootDiameter,base};
}
function gear(options){
 const d=gearDimensions(options),x=numeric(options.x,'X',0,2000),y=numeric(options.y,'Y',0,2000),rotation=numeric(options.rotation??0,'Rotation',-360,360)*Math.PI/180;
 const rp=d.pitch/2,rb=d.base/2,ra=d.outside/2,rf=d.root/2,cx=x+ra,cy=y+ra,alpha=d.pressureAngle*Math.PI/180;
 const inv=r=>{const t=Math.sqrt(Math.max(0,(r/rb)**2-1));return t-Math.atan(t);};
 const half=Math.PI/(2*d.teeth)-d.backlash/(2*rp),baseHalf=half+Math.tan(alpha)-alpha;
 const flank=r=>baseHalf-inv(r),start=Math.max(rf,rb),tipHalf=flank(ra),rootHalf=flank(start),step=2*Math.PI/d.teeth;
 if(tipHalf<=0||rootHalf>=step/2)throw Error('Parameters produce pointed or overlapping teeth');
 const p=[],point=(r,a)=>p.push([cx+r*Math.cos(a+rotation),cy+r*Math.sin(a+rotation)]);
 function arc(r,a,b){const n=Math.max(2,Math.ceil((b-a)*r/.1));for(let i=1;i<=n;i++)point(r,a+(b-a)*i/n);}
 // Sample involute flanks by radius, with pitch circle included for exact tooth thickness.
 const radii=[start,rp,ra];for(let i=1;i<40;i++)radii.push(start+(ra-start)*i/40);radii.sort((a,b)=>a-b);
 for(let t=0;t<d.teeth;t++){const a=t*step;point(rf,a-rootHalf);if(start>rf)point(start,a-rootHalf);for(const r of radii)point(r,a-flank(r));arc(ra,a-tipHalf,a+tipHalf);for(const r of [...radii].reverse())point(r,a+flank(r));if(start>rf)point(rf,a+rootHalf);arc(rf,a+rootHalf,a+step-rootHalf);}
 return d.bore>0?[hole(cx,cy,d.bore),closed(p)]:[closed(p)];
}
const api={polygon,gear,gearDimensions};root.CNCShapes=api;if(typeof module!=='undefined')module.exports=api;
})(globalThis);
