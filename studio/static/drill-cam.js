/* Explicit, portable plunge/peck cycles and sampled stock contact for offline CAM. */
(function(root){
  'use strict';
  const distance=(a,b)=>Math.hypot(a[0]-b[0],a[1]-b[1]);
  function centerOf(path,diameter) {
    const ring=[path[0]];
    for(const p of path.slice(1)) if(distance(ring.at(-1),p)>1e-8) ring.push(p);
    const a=ring[0];
    if(ring.length===1) return a;
    if(ring.length<9 || distance(a,ring.at(-1))>1e-6) throw Error('Drill requires point markers or closed circular hole outlines');
    ring.pop();
    let b=a,far=0;
    for(const p of ring){const d=distance(a,p);if(d>far){far=d;b=p;}}
    const u=b[0]-a[0],v=b[1]-a[1];
    let c=a,area=0;
    for(const p of ring){const cross=Math.abs(u*(p[1]-a[1])-v*(p[0]-a[0]));if(cross>area){area=cross;c=p;}}
    const m=c[0]-a[0],n=c[1]-a[1],det=2*(u*n-v*m);
    if(Math.abs(det)<1e-12) throw Error('Drill outline is degenerate');
    const ab=u*u+v*v,ac=m*m+n*n,center=[a[0]+(n*ab-v*ac)/det,a[1]+(u*ac-m*ab)/det],r=diameter/2;
    if(ring.some(p=>Math.abs(distance(p,center)-r)>Math.max(.0001,.002*r))) throw Error('Drill outline must be circular and match the cutter diameter; use a profile or pocket for a larger hole');
    let winding=0,travel=0;
    for(let i=0;i<ring.length;i++){
      const p=ring[i],q=ring[(i+1)%ring.length];
      const theta=Math.atan2(q[1]-center[1],q[0]-center[0])-Math.atan2(p[1]-center[1],p[0]-center[0]);
      const angle=Math.atan2(Math.sin(theta),Math.cos(theta));
      if(Math.abs(angle)>Math.PI/4+1e-5) throw Error('Drill outline must trace one complete circle without reversing');
      winding+=angle;travel+=Math.abs(angle);
    }
    if(Math.abs(Math.abs(winding)-2*Math.PI)>1e-5 || travel>2*Math.PI+1e-5) throw Error('Drill outline must trace one complete circle without reversing');
    return center;
  }
  function generate(p,paths,value,integer,protocols) {
    if(p.mode!=='router' || p.path_depths!=null || p.path_powers!=null) throw Error('Drilling requires a router and a single target depth');
    const diameter=value(p,'tool_diameter',3.175,.01,100), depth=value(p,'depth',1,.01,50);
    const safe=value(p,'safe',5,.1,100), plunge=value(p,'plunge',100,1,1000), rpm=value(p,'power',undefined,1,100000);
    const peck=value(p,'drill_peck',0,0,50),angle=value(p,'drill_point_angle',118,60,170),shape=p.tool_shape??'drill';
    if(peck && peck<.01) throw Error('Use zero for one plunge or a peck depth of at least 0.01 mm');
    if(!['drill','flat'].includes(shape)) throw Error('Drilling requires a drill bit or centre-cutting flat end mill');
    const passes=peck?Math.max(1,Math.ceil(depth/peck-1e-10)):1;
    if(paths.length*(2*passes+1)+10>200000) throw Error('Drilling exceeds the line limit; increase the peck depth or reduce hole count');
    const centers=paths.map(path=>centerOf(path,diameter)),r=diameter/2;
    if(centers.some(([x,y])=>x-r < -1e-8 || y-r < -1e-8 || x+r > +p.width+1e-8 || y+r > +p.height+1e-8)) throw Error('Drill footprint crosses the work area boundary');
    const retract='G0 Z'+safe.toFixed(3),lines=['G21','G90','G17','G94','G54','M5',retract,'M3 S'+integer(rpm),'G4 P2'],segments=[];
    let length=0;
    for(const [x,y] of centers){
      lines.push('G0 X'+x.toFixed(3)+' Y'+y.toFixed(3));
      for(let i=1;i<=passes;i++){
        const z=i===passes?-depth:-i*peck;
        lines.push('G1 Z'+z.toFixed(3)+' F'+plunge.toFixed(1),retract);
        segments.push([x,y,x,y,z,null,safe]);
        length+=safe-z;
      }
    }
    lines.push('M5');
    const xs=centers.map(p=>p[0]),ys=centers.map(p=>p[1]),protocol=p.protocol??'grbl';
    return {lines:protocols.postprocess(lines,protocol,'router',p.dwell_units??''),segments,
      cut_length:Math.round(length*100)/100, cut_minutes:Math.round(length/plunge*100)/100,mode:'router',
      bounds:[Math.min(...xs),Math.min(...ys),Math.max(...xs),Math.max(...ys)],
      warnings:[protocols.get(protocol).notes,'Depth is measured to the tool tip. Drilling time includes feed plunges from clearance; rapid retracts and spindle startup are excluded.'],
      drill_holes:centers.map(([x,y])=>[x,y,diameter]),simulation_tool:{diameter,shape,angle},prepared_paths:paths,prepared_depths:null};
  }
  function simulate(data,value) {
    const segments=data.segments;
    if(!Array.isArray(segments)||segments.length>200000) throw Error('Invalid segment count');
    const x=value(data,'x',0,0,2000),y=value(data,'y',0,0,2000),width=value(data,'width',100,.1,2000),height=value(data,'height',100,.1,2000);
    const n=Math.floor(value(data,'resolution',48,16,96)),stock=value(data,'stock_thickness',10,.01,200);
    const radius=value(data,'tool_diameter',3.175,.01,100)/2,shape=data.tool_shape||'flat',angle=value(data,'v_angle',60,10,170);
    if(!['flat','ball','v','drill'].includes(shape)) throw Error('Unknown simulated cutter');
    const heights=Array.from({length:n},()=>Array(n).fill(0)),dx=width/(n-1),dy=height/(n-1);
    let checks=0;
    for(const segment of segments){
      if(!Array.isArray(segment)||segment.length<5) throw Error('Invalid preview segment');
      const [ax,ay,bx,by,z]=segment,start=segment[6]??z;
      if(![ax,ay,bx,by,z,start].every(Number.isFinite)) throw Error('Non-finite segment');
      if(Math.min(z,start)<-stock) throw Error('Cut exceeds simulated stock thickness');
      const iMin=Math.max(0,Math.floor((Math.min(ax,bx)-x-radius)/dx)),iMax=Math.min(n-1,Math.ceil((Math.max(ax,bx)-x+radius)/dx));
      const jMin=Math.max(0,Math.floor((Math.min(ay,by)-y-radius)/dy)),jMax=Math.min(n-1,Math.ceil((Math.max(ay,by)-y+radius)/dy));
      if(iMax<iMin || jMax<jMin) continue;
      checks+=(iMax-iMin+1)*(jMax-jMin+1);
      if(checks>8000000) throw Error('Simulation too large; lower preview resolution');
      const vx=bx-ax,vy=by-ay,length2=vx*vx+vy*vy;
      for(let j=jMin;j<=jMax;j++) for(let i=iMin;i<=iMax;i++) {
        const px=x+i*dx-ax,py=y+j*dy-ay,t=length2?Math.max(0,Math.min(1,(px*vx+py*vy)/length2)):0;
        const d=Math.hypot(px-t*vx,py-t*vy);
        if(d>radius) continue;
        const sag=shape==='flat'?0:shape==='ball'?radius-Math.sqrt(Math.max(0,radius*radius-d*d)):d/Math.tan(angle*Math.PI/360);
        heights[j][i]=Math.min(heights[j][i],(length2?start+t*(z-start):Math.min(start,z))+sag);
      }
    }
    return {heights,x,y,width,height,resolution:n,stock_thickness:stock,warnings:['Sampled stock preview. Features smaller than a grid cell may disappear.']};
  }
  const api={generate,simulate};
  if(typeof module!=='undefined'&&module.exports) module.exports=api;
  root.CNCDrilling=api;
})(globalThis);
