/* Curved and compensated entry planning stays with polygon CAM on the server.
   Local-file CAM can ramp on an existing closed centreline using its XY geometry. */
(function(root){
 'use strict';
 const gap=(a,b)=>Math.hypot(b[0]-a[0],b[1]-a[1]);
 function properContour(path) {
   const points=[path[0]];
   for(const p of path.slice(1)) if(gap(p,points.at(-1))>1e-9) points.push(p);
   if(points.length<4 || gap(points[0],points.at(-1))>1e-6) throw Error('Contour ramp requires a closed machining contour');
   points[points.length-1]=points[0];
   const side=(a,b,c)=>(b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0]);
   const boundsOverlap=(a,b,c,d,k)=>Math.max(a[k],b[k])+1e-10>=Math.min(c[k],d[k]) && Math.max(c[k],d[k])+1e-10>=Math.min(a[k],b[k]);
   if(points.length*(points.length-1)/2>2000000) throw Error('Use the app server to check this large contour before ramping');
   for(let i=1;i<points.length;i++){
     const a=points[i-1],b=points[i],next=points[i+1]??points[1];
     if(Math.abs(side(a,b,next))<1e-10 && (b[0]-a[0])*(next[0]-b[0])+(b[1]-a[1])*(next[1]-b[1])<0) throw Error('Repair reversing contour edges before ramping');
     for(let j=i+2;j<points.length;j++){
       if(i===1 && j===points.length-1) continue;
       const c=points[j-1],d=points[j];
       if(boundsOverlap(a,b,c,d,0)&&boundsOverlap(a,b,c,d,1)&&side(a,b,c)*side(a,b,d)<=0&&side(c,d,a)*side(c,d,b)<=0) throw Error('Repair crossing or degenerate machining contours before using a ramp');
     }
   }
   return points;
 }
 function generate(p,clean,value,integer,protocols) {
   if(p.router_entry!=='ramp' || (p.operation||'line')!=='line') throw Error('Compensated ramp and pocket helix entries require the Studio server');
   if((p.tool_shape||'flat')!=='flat' || p.path_depths!=null) throw Error('A contour ramp requires a flat end mill and one target depth');
   const diameter=value(p,'tool_diameter',3.175,.01,100),depth=value(p,'depth',1,.01,50),step=value(p,'step',.5,.01,10);
   const safe=value(p,'safe',5,.1,100),feed=value(p,'feed',undefined,1,10000),plunge=value(p,'plunge',100,1,1000),rpm=value(p,'power',undefined,1,100000);
   const angle=value(p,'entry_angle',3,.25,15);
   if(p.stock_thickness!=null && depth>value(p,'stock_thickness',10,.01,200)+1e-8) throw Error('Entry depth exceeds the entered stock thickness');
   const profiles=clean.map(path=>{
     const points=properContour(path),lengths=points.slice(1).map((v,i)=>gap(v,points[i]));
     if(points.some(([x,y])=>x-diameter/2 < -1e-8 || y-diameter/2 < -1e-8 || x+diameter/2> +p.width+1e-8 || y+diameter/2> +p.height+1e-8))
       throw Error('Router entry cutter footprint extends outside the work area');
     return {points,lengths,perimeter:lengths.reduce((a,b)=>a+b,0)};
   });
   const lines=['G21','G90','G17','G94','G54','M5','G0 Z'+safe.toFixed(3),'M3 S'+integer(rpm),'G4 P2'];
   const segments=[],bounds=[Infinity,Infinity,-Infinity,-Infinity],slope=Math.tan(angle*Math.PI/180);
   let length=0,minutes=0,previous=0,state;
   function move(x,y,z,rate){
     if(lines.length>=199996) throw Error('Entry exceeds the 200000-line limit');
     const [ax,ay,az]=state;if(x===ax&&y===ay&&z===az) return;
     lines.push('G1 X'+x.toFixed(3)+' Y'+y.toFixed(3)+' Z'+z.toFixed(3)+' F'+rate.toFixed(1));
     segments.push([ax,ay,x,y,z,null,az]);const d=Math.hypot(x-ax,y-ay,z-az);length+=d;minutes+=d/rate;
     bounds[0]=Math.min(bounds[0],ax,x);bounds[1]=Math.min(bounds[1],ay,y);
     bounds[2]=Math.max(bounds[2],ax,x);bounds[3]=Math.max(bounds[3],ay,y);state=[x,y,z];
   }
   for(let level=1;level<=Math.ceil(depth/step);level++){
     const bottom=-Math.min(depth,level*step),drop=previous-bottom;
     if(drop<1e-10) continue;
     for(const {points,lengths,perimeter} of profiles){
       const [x,y]=points[0], turns=Math.max(1,Math.ceil(drop/(slope*perimeter)));
       if(lines.length+turns*lengths.length>200000) throw Error('Contour ramp exceeds the path segment limit');
       lines.push('G0 Z'+safe.toFixed(3),'G0 X'+x.toFixed(3)+' Y'+y.toFixed(3));
       state=[x,y,safe];move(x,y,previous,plunge);
       let along=0;
       for(let lap=0;lap<turns;lap++) for(let j=1;j<points.length;j++){
         along+=lengths[j-1];move(...points[j],Math.max(bottom,previous-drop*along/(turns*perimeter)),Math.min(feed,plunge));
       }
       for(const pt of points.slice(1)) move(...pt,bottom,feed);
       lines.push('G0 Z'+safe.toFixed(3));
     }
     previous=bottom;
   }
   lines.push('M5');
   return {lines:protocols.postprocess(lines,p.protocol??'grbl','router',p.dwell_units??''),segments,mode:'router',bounds,
     cut_length:Math.round(length*100)/100,cut_minutes:Math.round(minutes*100)/100,prepared_paths:clean,prepared_depths:null,
     warnings:[protocols.get(p.protocol??'grbl').notes,'Entry angle is a maximum. Ramping uses the plunge feed limit. Inspect the workholding and tool ramp capability.']};
 }
 const api={generate};if(typeof module!=='undefined'&&module.exports) module.exports=api;root.CNCRouterEntry=api;
})(globalThis);
