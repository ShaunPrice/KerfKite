/* Dependency-free offline centreline CAM. No controller or network access. */
(function (root) {
  'use strict';
  const MAX = 200000;
  const protocols=root.CNCProtocols || (typeof require==='function'?require('./protocols.js'):null);
  const routerEntry=root.CNCRouterEntry || (typeof require==='function'?require('./router-entry.js'):null);
  const drilling=root.CNCDrilling || (typeof require==='function'?require('./drill-cam.js'):null);
  const fail = message => { throw new Error(message); };
  function number(value, name, lo, hi) {
    if (value === null || typeof value === 'boolean' || !['number', 'string'].includes(typeof value) || (typeof value === 'string' && !value.trim())) fail(name + ' must be a number');
    const n = Number(value);
    if (!Number.isFinite(n) || n < lo || n > hi) fail(name + ' must be between ' + lo + ' and ' + hi);
    return n;
  }
  const value = (p, key, fallback, lo, hi) => number(p[key] === undefined ? fallback : p[key], key, lo, hi);
  const distance = (a, b) => Math.hypot(b[0]-a[0], b[1]-a[1]);
  // Python's integer formatting uses ties-to-even for commanded S values.
  const integer = n => { const lo=Math.floor(n);return String(n-lo===.5 ? lo+(lo%2) : Math.round(n)); };
  const same = (a, b) => a[0] === b[0] && a[1] === b[1];
  function knife(paths, offset, lead) {
    let count = 0;
    return paths.map(path => {
      // A point is kept only when it is further than 0.1 nm from the last point kept (studio/operations.py
      // knife_points), so every move that remains has a direction and none has zero length.
      const p = [];
      for (const v of path) if (!p.length || distance(p[p.length-1], v) > 1e-7) p.push(v);
      if (p.length < 2) fail('Knife paths must contain nonzero travel');
      const vectors = p.slice(1).map((b, i) => { const a=p[i], d=distance(a,b); return [(b[0]-a[0])/d,(b[1]-a[1])/d]; });
      const [ux,uy]=vectors[0];
      const q=[[p[0][0]+(offset-lead)*ux,p[0][1]+(offset-lead)*uy],[p[0][0]+offset*ux,p[0][1]+offset*uy]];
      vectors.forEach(([vx,vy], i) => {
        const [x,y]=p[i+1]; q.push([x+offset*vx,y+offset*vy]);
        const next=vectors[i+1] || (same(p[p.length-1],p[0]) ? vectors[0] : null);
        if (next) {
          const a=Math.atan2(vy,vx), b=Math.atan2(next[1],next[0]);
          const delta=((b-a+Math.PI)%(2*Math.PI)+2*Math.PI)%(2*Math.PI)-Math.PI;
          const n=Math.max(1,Math.ceil(Math.abs(delta)/(Math.PI/36)));
          for(let j=1;j<=n;j++) q.push([x+offset*Math.cos(a+delta*j/n),y+offset*Math.sin(a+delta*j/n)]);
        }
        if (count+q.length>100000) fail('Knife compensation exceeds vertex limit');
      });
      count+=q.length;return q;
    });
  }
  function vectorLaserOptions(project) {
    const pattern=project.laser_pattern === undefined ? 'continuous' : project.laser_pattern;
    const shape=project.lead_shape === undefined ? 'straight' : project.lead_shape;
    if (!['continuous','perforate','bridges'].includes(pattern)) fail('Choose continuous cutting, perforation or holding bridges');
    if (!['straight','arc-left','arc-right'].includes(shape)) fail('Choose a straight, left or right tangent lead');
    if (pattern !== 'continuous' && project.path_powers != null) fail('Laser perforation and bridges require vector line or profile operations');
    const options={pattern,shape};
    if (pattern !== 'continuous') options.gap=value(project,'laser_gap',1,.01,100);
    if (pattern === 'perforate') options.dash=value(project,'laser_dash',3,.01,1000);
    if (pattern === 'bridges') {
      options.count=value(project,'laser_bridge_count',4,1,1000);
      if (!Number.isInteger(options.count)) fail('Bridges per contour must be a whole number');
    }
    return options;
  }
  function leadPoints(a,b,length,shape,entering) {
    if (length<1e-9) return [];
    const edge=distance(a,b), tx=(b[0]-a[0])/edge, ty=(b[1]-a[1])/edge;
    const [x,y]=entering?a:b, sign=entering?-1:1;
    if (shape==='straight') return [[x+sign*length*tx,y+sign*length*ty]];
    const radius=2*length/Math.PI, side=shape==='arc-left'?1:-1;
    const step=Math.min(Math.PI/18,2*Math.acos(Math.max(-1,1-.01/radius)));
    const count=Math.ceil(Math.PI/2/step), points=[];
    for (let i=1;i<=count;i++) {
      const angle=(Math.PI/2)*i/count, along=sign*radius*Math.sin(angle), across=side*radius*(1-Math.cos(angle));
      points.push([x+along*tx-across*ty,y+along*ty+across*tx]);
    }
    return entering?points.reverse():points;
  }
  function gapBoundaries(perimeter,offset,points,cut) {
    const boundaries=[];
    if (cut.pattern==='continuous') return boundaries;
    if (cut.pattern==='bridges') {
      if (points.length<4 || distance(points[0],points.at(-1))>1e-7) fail('Laser holding bridges require closed contours');
      const pitch=perimeter/cut.count;
      if (cut.gap>=pitch-1e-9) fail('Holding bridges cover the contour; reduce their width or count');
      for (let i=0;i<cut.count;i++) {
        const start=(i+.5)*pitch-cut.gap/2;
        boundaries.push(offset+start,offset+(start+cut.gap));
      }
    } else {
      const pitch=cut.dash+cut.gap, count=Math.ceil(perimeter/pitch);
      if (count>100000) fail('Perforation exceeds the segment limit; increase the cut or gap length');
      for (let i=0;i<count;i++) {
        const start=i*pitch+cut.dash;
        if (start<perimeter) boundaries.push(offset+start,offset+Math.min(perimeter,start+cut.gap));
      }
    }
    return boundaries;
  }
  function laser(path,power,options,width,height,cut) {
    const contour=[];
    for(const point of path) if(!contour.length || distance(contour.at(-1),point)>1e-9) contour.push(point.slice());
    if(contour.length<2) fail('Laser paths must contain nonzero travel');
    const [leadIn,leadOut,rampIn,rampOut,start,end]=options;
    const before=leadPoints(contour[0],contour[1],leadIn,cut.shape,true);
    const after=leadPoints(contour.at(-2),contour.at(-1),leadOut,cut.shape,false);
    const points=before.concat(contour,after);
    if(points.some(([x,y])=>x<0 || x>width || y<0 || y>height)) fail('Laser lead-in/out extends outside the work area');
    const lengths=points.slice(1).map((p,i)=>distance(points[i],p));
    const sum=arr=>arr.reduce((a,b)=>a+b,0), total=sum(lengths);
    if(rampIn+rampOut>total+1e-9) fail('Power ramp distances overlap');
    const offset=sum(lengths.slice(0,before.length)),perimeter=sum(lengths.slice(before.length,before.length+contour.length-1));
    const gaps=gapBoundaries(perimeter,offset,contour,cut);
    const stops=[...new Set([...gaps,rampIn,total-rampOut])].sort((a,b)=>a-b);
    let gapIndex=0, stopIndex=0;
    // Power samples and stops are consumed in increasing arclength order.
    function level(d) {
      while(gapIndex<gaps.length && gaps[gapIndex]<=d) gapIndex++;
      if(gapIndex%2) return 0;
      return rampIn && d<rampIn ? power*(start+(100-start)*d/rampIn)/100 : rampOut && d>total-rampOut ? power*(end+(100-end)*(total-d)/rampOut)/100 : power;
    }
    const output=[[...points[0],level(0)]];let travelled=0;
    lengths.forEach((length,index)=>{
      if(!length) return;
      const a=points[index],b=points[index+1],stop=travelled+length;
      const cuts=[travelled];
      while(stopIndex<stops.length && stops[stopIndex]<stop) {
        const boundary=stops[stopIndex++];
        if(boundary>travelled) cuts.push(boundary);
      }
      cuts.push(stop);
      for(let c=1;c<cuts.length;c++) {
        const lo=cuts[c-1],hi=cuts[c],changing=(rampIn && lo<rampIn)||(rampOut && hi>total-rampOut);
        const count=changing ? Math.max(1,Math.ceil((hi-lo)/.5)) : 1;
        if(output.length+count>MAX) fail('Laser path exceeds segment limit');
        for(let i=0;i<count;i++) { const s0=lo+(hi-lo)*i/count,s1=lo+(hi-lo)*(i+1)/count,t=(s1-travelled)/length;output.push([a[0]+(b[0]-a[0])*t,a[1]+(b[1]-a[1])*t,level((s0+s1)/2)]); }
      }
      travelled=stop;
    });
    return output;
  }
  function generate(p) {
    if(!p || typeof p!=='object' || Array.isArray(p)) fail('Project must be an object');
    if(p.tabs!==undefined&&p.tabs!==null&&p.tabs!==false)fail('Holding tabs require the app server; offline CAM will not omit them.');
    if(Object.prototype.hasOwnProperty.call(p,'operations')) {
      if(!Array.isArray(p.operations) || p.operations.length<1 || p.operations.length>100) fail('Use 1–100 ordered operations');
      if(p.mode==='router'){
        const tools=p.operations.map(op=>{const v={...p,...op},shape=v.operation==='vcarve'?'v':v.tool_shape||(v.operation==='drill'?'drill':'flat');return JSON.stringify([v.tool_id??null,v.tool_diameter??3.175,shape,shape==='v'?(v.v_angle??60):shape==='drill'?(v.drill_point_angle??118):null]);});
        if(new Set(tools).size>1)fail('Tools differ between operations; generate separate manual tool-change stages using the app server');
      }
      const base={...p};for(const key of ['operations','paths','path_depths','path_powers']) delete base[key];
      const jobs=[];let lineCount=0;
      for(const operation of p.operations) {
        if(!operation || typeof operation!=='object' || Array.isArray(operation) || Object.prototype.hasOwnProperty.call(operation,'operations')) fail('Nested or invalid operations are unsupported');
        for(const key of ['mode','protocol','width','height','dwell_units']) if(Object.prototype.hasOwnProperty.call(operation,key) && operation[key]!==p[key]) fail('An ordered job must use one head, protocol and work area');
        const job=generate({...base,...operation});lineCount+=job.lines.length;
        if(lineCount>MAX) fail('Combined job exceeds line limit');
        jobs.push(job);
      }
      return {lines:jobs.flatMap(job=>job.lines.filter(l=>l!=='M30')).concat(jobs.at(-1).lines.at(-1)==='M30'?['M30']:[]),segments:jobs.flatMap(job=>job.segments),
        cut_length:Math.round(jobs.reduce((n,job)=>n+job.cut_length,0)*100)/100,
        cut_minutes:Math.round(jobs.reduce((n,job)=>n+job.cut_minutes,0)*100)/100,mode:p.mode,
        bounds:[Math.min(...jobs.map(job=>job.bounds[0])),Math.min(...jobs.map(job=>job.bounds[1])),Math.max(...jobs.map(job=>job.bounds[2])),Math.max(...jobs.map(job=>job.bounds[3]))],
        warnings:[...new Set(jobs.flatMap(job=>job.warnings))],operations:jobs.length,
        ...(jobs.some(j=>j.drill_holes)?{drill_holes:jobs.flatMap(j=>j.drill_holes||[])}:{}),
        ...(jobs[0].simulation_tool&&jobs.every(j=>JSON.stringify(j.simulation_tool)===JSON.stringify(jobs[0].simulation_tool))?{simulation_tool:jobs[0].simulation_tool}:{})};
    }
    const mode=p.mode,protocol=p.protocol===undefined?'grbl':p.protocol,op=p.operation===undefined?'line':p.operation;
    if(!['router','laser','knife','pen'].includes(mode)) fail('Choose router, laser or knife');
    const pen=mode==='pen',penServo=pen&&p.pen_lift==='servo';
    if(pen&&!penServo&&p.pen_lift!==undefined&&p.pen_lift!=='z') fail('Pen lift must be z or servo');
    const entry=p.router_entry??'plunge';
    if(mode==='router' && entry!=='plunge' && (entry!=='ramp'||op!=='line')) fail('Only a closed-line ramp is available offline. Use Studio server CAM for compensated ramp or pocket helix entries. Drilling uses its own peck controls.');
    if(op!=='line'&&op!=='drill') fail('Offline CAM supports line operations only for contour work, plus router drilling. Use hosted CNC Studio for '+String(op)+' CAM.');
    const profile=protocols.get(protocol);
    if(!profile.modes.includes(pen?'router':mode))fail(profile.name+' dynamic laser export is not implemented');  // a pen is checked as a router, which every profile has

    const width=value(p,'width',undefined,1,2000),height=value(p,'height',undefined,1,2000),feed=value(p,'feed',undefined,1,10000),power=value(p,'power',undefined,1,100000);
    const safe=value(p,'safe',5,.1,100),step=value(p,'step',.5,.01,10),plunge=value(p,'plunge',100,1,1000),passes=value(p,'passes',1,1,100);
    let depth=value(p,'depth',1,.01,50);
    if(!Number.isInteger(passes)) fail('Passes must be a whole number');
    if(!Array.isArray(p.paths) || p.paths.length<1 || p.paths.length>3000) fail('Add between 1 and 3000 paths');
    let count=0;
    let clean=p.paths.map(path=>{
      if(!Array.isArray(path) || path.length<2) fail('Each path needs at least two points');
      count+=path.length;if(count>100000) fail('Too many vertices; simplify the design');
      return path.map(point=>{if(!Array.isArray(point)||point.length!==2) fail('Invalid XY point');return [number(point[0],'X',0,width),number(point[1],'Y',0,height)];});
    });
    if(mode==='router'&&entry!=='plunge') return routerEntry.generate(p,clean,value,integer,protocols);
    if(op==='drill') return drilling.generate(p,clean,value,integer,protocols);
    if(mode==='router' && p.tool_shape==='drill') fail('Choose a milling cutter for a contour or pocket; a drill bit is for drilling');
    const warnings=['Offline centreline CAM; inspect the exported toolpath before machining.','Review tool geometry, stock and clearance before running.','Time estimate excludes travel, plunge, acceleration and spindle startup.'];
    let depths=p.path_depths===undefined ? null : p.path_depths;
    if(mode==='knife') {
      if(depths!==null) fail('Per-vertex knife depth paths are not supported');
      const offset=value(p,'knife_offset',.25,.01,5),lead=value(p,'knife_lead',1,offset,20);
      clean=knife(clean,offset,lead);
      for(const path of clean) for(const [x,y] of path) { number(x,'Compensated X',0,width);number(y,'Compensated Y',0,height); }
      count=clean.reduce((n,path)=>n+path.length,0);
      warnings.push('Knife alignment lead-in marks material before each contour. Test blade offset and corner swivels on scrap.');
    }
    if(depths!==null) {
      if((mode!=='router'&&!pen) || !Array.isArray(depths) || depths.length!==clean.length) fail('Depth paths must match router XY paths');
      depths=depths.map((row,i)=>{if(!Array.isArray(row)||row.length!==clean[i].length) fail('Each vertex must have a matching depth');return row.map(z=>number(z,'Vertex Z',-50,0));});
      depth=0;for(const row of depths) for(const z of row) depth=Math.max(depth,-z);
      if(depth<=0) fail('No contour cuts below the stock surface');
    }
    if(pen) {
      // A broad nib drawn with a fine pen: each stroke again and again, side by side along the nib's edge.
      if(penServo&&depths!==null) fail('Drawing with depths at each point needs the Z axis to lift the pen');
      const nib=value(p,'pen_nib',0,0,20);
      if(nib>0) {
        const tip=value(p,'pen_tip',.4,.1,5),angle=value(p,'pen_nib_angle',45,-360,360)*Math.PI/180;
        const copies=Math.max(2,Math.ceil(nib/tip-1e-9)+1),out=[],outDepths=[];let total=0;
        clean.forEach((path,index)=>{
          for(let k=0;k<copies;k++) {
            const along=-nib/2+nib*k/(copies-1);
            const copy=path.map(([x,y])=>[x+along*Math.cos(angle),y+along*Math.sin(angle)]);
            for(const [x,y] of copy) if(x<0||x>width||y<0||y>height) fail("The broad nib takes the drawing outside the work area: move it in by half the nib's width");
            const row=depths===null?null:depths[index].slice();
            if(k%2) {copy.reverse();if(row)row.reverse();}
            total+=copy.length;if(total>100000) fail('Too many vertices; use a narrower nib, a wider pen line or a simpler design');
            out.push(copy);if(row)outDepths.push(row);
          }
        });
        clean=out;if(depths!==null)depths=outDepths;count=total;
      }
      warnings.push(penServo?"The pen is lifted by the spindle or laser output and Z is not moved: set the holder's height by hand, and check which S values lift and lower it before putting a pen in.":"The spindle stays off. Z zero is where the pen touches the paper; the depth is how far the holder's spring is pressed.",'Take the cutter or the laser off before drawing, and test on scrap paper first.');
    }
    const pixelPowers=p.path_powers===undefined ? null : p.path_powers;
    if(pixelPowers!==null && mode!=='laser') fail('Pixel powers require laser mode');
    let laserPaths=[],options=[];
    if(mode==='laser') {
      options=[['lead_in',0,100],['lead_out',0,100],['ramp_in',0,1000],['ramp_out',0,1000],['start_power',100,100],['end_power',100,100]].map(([key,d,max])=>value(p,key,d,0,max));
      const cut=vectorLaserOptions(p);
      if(pixelPowers!==null) {
        if(options.slice(0,4).some(Boolean)) fail('Raster powers require lead/ramp distances zero');
        if(!Array.isArray(pixelPowers) || pixelPowers.length!==clean.length) fail('Pixel powers must match raster paths');
      }
      let expanded=0;
      laserPaths=clean.map((path,index)=>{
        let out;
        if(pixelPowers===null) out=laser(path,power,options,width,height,cut);
        else {
          const powers=pixelPowers[index];
          if(!Array.isArray(powers)||powers.length!==path.length) fail('Pixel powers must match vertices');
          out=path.map(([x,y],i)=>[x,y,number(powers[i],'Pixel power',0,power)]);
        }
        expanded+=out.length;if(expanded*passes>MAX) fail('Laser job exceeds 200000 segments');return out;
      });
      if(options.slice(0,4).some(Boolean)) warnings.push('Tangent leads are cutting moves and may mark outside the design. Power ramps use steps of at most 0.5 mm; M4 also scales power with speed.');
    }
    const levels=mode==='laser'?Array(passes).fill(null):pen?Array(passes).fill(-depth):Array.from({length:Math.ceil(depth/step)},(_,i)=>-Math.min(depth,step*(i+1)));
    if(count*levels.length>MAX) fail('Job exceeds 200000 cutting segments');
    let lines=['G21','G90','G17','G94','G54','M5'];
    if(mode!=='laser'&&!penServo) {lines.push('G0 Z'+safe.toFixed(3));if(mode==='router') lines.push('M3 S'+integer(power),'G4 P2');}
    const segments=[];let length=0;
    let penUp=0;
    if(penServo) {
      // The holder is worked from the spindle or laser output; Z is not moved at all.
      const pause=value(p,'pen_dwell',.2,0,5);penUp=value(p,'pen_up_power',0,0,100000);
      const configured=profile.id==='mach3'||profile.id==='uccnc';
      if(configured&&pause>0&&p.dwell_units!=='seconds'&&p.dwell_units!=='milliseconds') fail('Select configured dwell units: seconds or milliseconds');
      const milliseconds=profile.family==='marlin'||(configured&&p.dwell_units==='milliseconds');
      const wait='G4 P'+(milliseconds?integer(pause*1000):pause.toFixed(3)),up=penUp>0?'M3 S'+integer(penUp):'M5',down='M3 S'+integer(power);
      const holder=command=>{lines.push(command);if(pause>0)lines.push(wait);};
      holder(up);
      for(const _ of levels) for(const path of clean) {
        let [x,y]=path[0];
        lines.push(`G0 X${x.toFixed(3)} Y${y.toFixed(3)}`);holder(down);
        for(let i=1;i<path.length;i++) {
          const [nx,ny]=path[i];
          lines.push(`G1 X${nx.toFixed(3)} Y${ny.toFixed(3)} F${feed.toFixed(1)}`);
          segments.push([x,y,nx,ny,0,power,0]);length+=Math.hypot(nx-x,ny-y);x=nx;y=ny;
        }
        holder(up);
      }
    }
    for(const z of penServo?[]:levels) clean.forEach((original,pathIndex)=>{
      const path=mode==='laser'?laserPaths[pathIndex]:original.map(([x,y])=>[x,y,null]);
      let [x,y]=path[0],currentZ=depths!==null?Math.max(z,depths[pathIndex][0]):z;
      if(mode!=='laser') lines.push('G0 Z'+safe.toFixed(3),`G0 X${x.toFixed(3)} Y${y.toFixed(3)}`,`G1 Z${currentZ.toFixed(3)} F${plunge.toFixed(1)}`);
      else lines.push('M5',`G0 X${x.toFixed(3)} Y${y.toFixed(3)}`,'M4 S'+integer(path[0][2]));
      for(let i=1;i<path.length;i++) {
        const [nx,ny,segmentPower]=path[i],nextZ=depths!==null?Math.max(z,depths[pathIndex][i]):currentZ;
        let suffix=mode==='laser'?' S'+integer(segmentPower):'';
        if(depths!==null) suffix+=' Z'+nextZ.toFixed(3);
        lines.push(`G1 X${nx.toFixed(3)} Y${ny.toFixed(3)} F${(depths!==null?Math.min(feed,plunge):feed).toFixed(1)}`+suffix);
        segments.push([x,y,nx,ny,nextZ||0,segmentPower,currentZ||0]);
        length+=Math.hypot(nx-x,ny-y,(nextZ||0)-(currentZ||0));currentZ=nextZ;x=nx;y=ny;
      }
      lines.push(mode==='laser'?'M5':'G0 Z'+safe.toFixed(3));
    });
    if(!(penServo&&penUp>0)) lines.push('M5');
    lines=protocols.postprocess(lines,protocol,pen?'router':mode,p.dwell_units===undefined?'':p.dwell_units);
    warnings.push(profile.notes);
    if(profile.family==='rs274')warnings.push('Before loading, clear controller scaling, rotation and local offsets. Set G54 Z zero for the current tool tip with tool-length compensation off. Exact-stop output may run slower on dense curves.');
    const envelope=mode==='laser'?laserPaths:clean,bounds=[Infinity,Infinity,-Infinity,-Infinity];
    for(const path of envelope) for(const [x,y] of path) {bounds[0]=Math.min(bounds[0],x);bounds[1]=Math.min(bounds[1],y);bounds[2]=Math.max(bounds[2],x);bounds[3]=Math.max(bounds[3],y);}
    return {lines,segments,cut_length:Math.round(length*100)/100,cut_minutes:Math.round(length/(depths!==null?Math.min(feed,plunge):feed)*100)/100,mode,bounds,warnings,prepared_paths:clean,prepared_depths:depths};
  }
  const api={generate,simulate:data=>drilling.simulate(data,value)};
  if(typeof module!=='undefined' && module.exports) module.exports=api;
  root.CNCOffline=api;
})(typeof globalThis!=='undefined'?globalThis:this);
