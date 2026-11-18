/* Dependency-free offline centreline CAM. No controller or network access. */
(function (root) {
  'use strict';
  const MAX = 200000;
  const protocols=root.CNCProtocols || (typeof require==='function'?require('./protocols.js'):null);
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
      const p = path.filter((v, i) => !i || distance(path[i-1], v) > 1e-7);
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
  function laser(path, power, options, width, height) {
    const points=[];
    for(const point of path) if(!points.length || distance(points[points.length-1],point)>1e-9) points.push(point.slice());
    if(points.length<2) fail('Laser paths must contain nonzero travel');
    const [leadIn,leadOut,rampIn,rampOut,start,end]=options;
    if(leadIn) { const [a,b]=points, d=distance(a,b);points.unshift(a.map((v,i)=>v-(b[i]-v)/d*leadIn)); }
    if(leadOut) { const a=points[points.length-2],b=points[points.length-1],d=distance(a,b);points.push(b.map((v,i)=>v+(v-a[i])/d*leadOut)); }
    if(points.some(([x,y])=>x<0 || x>width || y<0 || y>height)) fail('Laser lead-in/out extends outside the work area');
    const lengths=points.slice(1).map((p,i)=>distance(points[i],p)),total=lengths.reduce((a,b)=>a+b,0);
    if(rampIn+rampOut>total+1e-9) fail('Power ramp distances overlap');
    const level=d=>rampIn && d<rampIn ? power*(start+(100-start)*d/rampIn)/100 : rampOut && d>total-rampOut ? power*(end+(100-end)*(total-d)/rampOut)/100 : power;
    const output=[[...points[0],level(0)]];let travelled=0;
    lengths.forEach((length,index)=>{
      const a=points[index],b=points[index+1],stop=travelled+length;
      const cuts=[...new Set([travelled,stop,...[rampIn,total-rampOut].filter(v=>v>travelled && v<stop)])].sort((a,b)=>a-b);
      for(let c=1;c<cuts.length;c++) {
        const lo=cuts[c-1],hi=cuts[c],changing=(rampIn && lo<rampIn)||(rampOut && hi>total-rampOut);
        const count=changing ? Math.max(1,Math.ceil((hi-lo)/.5)) : 1;
        if(output.length+count>MAX) fail('Laser ramp exceeds segment limit');
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
        const tools=p.operations.map(op=>{const v={...p,...op},shape=v.operation==='vcarve'?'v':v.tool_shape||'flat';return JSON.stringify([v.tool_id??null,v.tool_diameter??3.175,shape,shape==='v'?(v.v_angle??60):null]);});
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
        warnings:[...new Set(jobs.flatMap(job=>job.warnings))],operations:jobs.length};
    }
    const mode=p.mode,protocol=p.protocol===undefined?'grbl':p.protocol,op=p.operation===undefined?'line':p.operation;
    if(!['router','laser','knife'].includes(mode)) fail('Choose router, laser or knife');
    if(op!=='line') fail('Offline CAM supports line operations only. Use hosted CNC Studio for '+String(op)+' CAM.');
    const profile=protocols.get(protocol);
    if(!profile.modes.includes(mode))fail(profile.name+' dynamic laser export is not implemented');

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
      if(mode!=='router' || !Array.isArray(depths) || depths.length!==clean.length) fail('Depth paths must match router XY paths');
      depths=depths.map((row,i)=>{if(!Array.isArray(row)||row.length!==clean[i].length) fail('Each vertex must have a matching depth');return row.map(z=>number(z,'Vertex Z',-50,0));});
      depth=0;for(const row of depths) for(const z of row) depth=Math.max(depth,-z);
      if(depth<=0) fail('No contour cuts below the stock surface');
    }
    const pixelPowers=p.path_powers===undefined ? null : p.path_powers;
    if(pixelPowers!==null && mode!=='laser') fail('Pixel powers require laser mode');
    let laserPaths=[],options=[];
    if(mode==='laser') {
      options=[['lead_in',0,100],['lead_out',0,100],['ramp_in',0,1000],['ramp_out',0,1000],['start_power',100,100],['end_power',100,100]].map(([key,d,max])=>value(p,key,d,0,max));
      if(pixelPowers!==null) {
        if(options.slice(0,4).some(Boolean)) fail('Raster powers require lead/ramp distances zero');
        if(!Array.isArray(pixelPowers) || pixelPowers.length!==clean.length) fail('Pixel powers must match raster paths');
      }
      let expanded=0;
      laserPaths=clean.map((path,index)=>{
        let out;
        if(pixelPowers===null) out=laser(path,power,options,width,height);
        else {
          const powers=pixelPowers[index];
          if(!Array.isArray(powers)||powers.length!==path.length) fail('Pixel powers must match vertices');
          out=path.map(([x,y],i)=>[x,y,number(powers[i],'Pixel power',0,power)]);
        }
        expanded+=out.length;if(expanded*passes>MAX) fail('Laser job exceeds 200000 segments');return out;
      });
      if(options.slice(0,4).some(Boolean)) warnings.push('Straight tangent leads are cutting moves and may mark outside the design. Power ramps use steps of at most 0.5 mm; M4 also scales power with speed.');
    }
    const levels=mode==='laser'?Array(passes).fill(null):Array.from({length:Math.ceil(depth/step)},(_,i)=>-Math.min(depth,step*(i+1)));
    if(count*levels.length>MAX) fail('Job exceeds 200000 cutting segments');
    let lines=['G21','G90','G17','G94','G54','M5'];
    if(mode!=='laser') {lines.push('G0 Z'+safe.toFixed(3));if(mode==='router') lines.push('M3 S'+integer(power),'G4 P2');}
    const segments=[];let length=0;
    for(const z of levels) clean.forEach((original,pathIndex)=>{
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
    lines.push('M5');
    lines=protocols.postprocess(lines,protocol,mode,p.dwell_units===undefined?'':p.dwell_units);
    warnings.push(profile.notes);
    if(profile.family==='rs274')warnings.push('Before loading, clear controller scaling, rotation and local offsets. Set G54 Z zero for the current tool tip with tool-length compensation off. Exact-stop output may run slower on dense curves.');
    const envelope=mode==='laser'?laserPaths:clean,bounds=[Infinity,Infinity,-Infinity,-Infinity];
    for(const path of envelope) for(const [x,y] of path) {bounds[0]=Math.min(bounds[0],x);bounds[1]=Math.min(bounds[1],y);bounds[2]=Math.max(bounds[2],x);bounds[3]=Math.max(bounds[3],y);}
    return {lines,segments,cut_length:Math.round(length*100)/100,cut_minutes:Math.round(length/(depths!==null?Math.min(feed,plunge):feed)*100)/100,mode,bounds,warnings,prepared_paths:clean,prepared_depths:depths};
  }
  const api={generate};
  if(typeof module!=='undefined' && module.exports) module.exports=api;
  root.CNCOffline=api;
})(typeof globalThis!=='undefined'?globalThis:this);
