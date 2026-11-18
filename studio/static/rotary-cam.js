/* Cylindrical posting for the standalone file editor. Port of studio.rotary.wrap_job. */
(function(root){
'use strict';
function number(value,label,low,high) {
  if(typeof value!=='number'||!Number.isFinite(value)||value<low||value>high)throw Error(`${label} must be a finite number in ${low}..${high}`);
  return value;
}
const mapping={A:['X','Z'],B:['Y','Z'],C:['Z','X']};
function wrap(job,o) {
  if(!job||typeof job!=='object'||!o||typeof o!=='object')throw Error('Supply a generated job and rotary setup.');
  const mode=job.mode,post=o.postprocessor,wrapAxis=o.wrap_axis,rotaryAxis=o.output_axis??'A',reference=o.feed_reference??'surface';
  if(!['linuxcnc','grblhal','xpro-v5'].includes(post))throw Error('Choose a supported rotary postprocessor.');
  if(!['router','laser'].includes(mode)||(post==='linuxcnc'&&mode==='laser'))throw Error('LinuxCNC supports rotary router export; for dynamic laser choose a supported laser postprocessor.');
  if(!Object.hasOwn(mapping,rotaryAxis)||post==='xpro-v5'&&rotaryAxis!=='A')throw Error('Choose a supported physical rotary axis.');
  if(!['X','Y'].includes(wrapAxis)||!['surface','cut'].includes(reference))throw Error('Choose a valid wrapped axis and feed reference.');
  const diameter=number(o.diameter,'Diameter',1,2000), radius=diameter/2, circumference=Math.PI*diameter;
  const offset=number(o.angle_offset??0,'Angle offset',-360000,360000),direction=number(o.direction??1,'Direction',-1,1);
  if(![-1,1].includes(direction))throw Error('Direction must be -1 or 1.');
  const [axialOut,radialOut]=mapping[rotaryAxis], axial=wrapAxis==='Y'?'X':'Y', laser=mode==='laser';
  const fixture=o.fixture;
  if(Object.hasOwn(o,'fixture')){
    if(!fixture||typeof fixture!=='object'||Array.isArray(fixture))throw Error('Supply a fixture envelope.');
    number(fixture.axial_min,'Minimum axial position',-2000,2000);number(fixture.axial_max,'Maximum axial position',-2000,2000);
    if(fixture.axial_min>=fixture.axial_max)throw Error('Fixture maximum must exceed its minimum.');
    number(fixture.min_radius,'Minimum tool-tip radius',0,radius);
  }
  let source=job.lines;
  if(!Array.isArray(source)||source.length<1||source.length>200000||source.some(l=>typeof l!=='string'||l.length>256))throw Error('Invalid or oversized rotary source.');
  if(source.at(-1)==='M30')source=source.slice(0,-1);
  const state={X:null,Y:null,Z:laser?0:null}, lines=[];
  let units=false, absolute=false, feedMode=false, inverseMode=false, on=false, lastFeed=null, moves=0,min=Infinity,max=-Infinity;
  let minAxial=Infinity,maxAxial=-Infinity,minZ=Infinity,maxZ=-Infinity,minutes=0;
  for(const raw of source){
    const tokens=raw.trim().split(/\s+/),words={};
    for(const token of tokens){const match=/^([A-Z])([+-]?(?:\d+(?:\.\d*)?|\.\d+))$/.exec(token);
      if(!match||Object.hasOwn(words,match[1]))throw Error('Rotary requires canonical generated G-code without comments or duplicate words.');
      words[match[1]]=number(Number(match[2]),'Source word',-1e9,1e9);
    }
    const command=tokens[0],keys=Object.keys(words);
    if(['G21','G90','G17','G54','G94','M5','G40','G49','G80','G61'].includes(command)&&tokens.length===1){
      if(post!=='linuxcnc'&&['G40','G49','G80','G61'].includes(command))throw Error('Source contains a LinuxCNC-only setup command.');
      if(command==='G21')units=true;
      if(command==='G90')absolute=true;
      if(command==='M5')on=false;
      if(command==='G94'){feedMode=true;inverseMode=post!=='xpro-v5';lines.push(inverseMode?'G93':'G94');}
      else lines.push(command);
      continue;
    }
    if(command===(laser?'M4':'M3')&&keys.length===2&&Object.hasOwn(words,'S')){
      number(words.S,'Power',laser?0:.001,100000);on=true;lines.push(raw.trim());continue;
    }
    if(!laser&&command==='G4'&&keys.length===2&&Object.hasOwn(words,'P')&&words.P>=0&&words.P<=60){lines.push(raw.trim());continue;}
    if(!['G0','G1'].includes(command))throw Error('Unsupported command for this rotary operation.');
    if(!units||!absolute||!feedMode)throw Error('Establish G21 G90 G94 before motion.');
    const cutting=command==='G1', allowed=laser?['G','X','Y']:['G','X','Y','Z'];
    if(cutting)allowed.push('F');if(cutting&&laser)allowed.push('S');
    const coords=['X','Y','Z'].filter(k=>Object.hasOwn(words,k));
    if(!coords.length||keys.some(k=>!allowed.includes(k)))throw Error('Unsupported rotary motion words.');
    if(laser&&!cutting&&on)throw Error('Laser must be off before a rapid.');
    if(cutting&&laser&&(!on||!Object.hasOwn(words,'S')))throw Error('Laser cutting requires M4 and an explicit power.');
    if(Object.hasOwn(words,'S'))number(words.S,'Laser power',0,100000);
    const next={...state};for(const k of coords)next[k]=words[k];
    if(next.Z!==null&&next.Z<=-radius)throw Error('Cut depth reaches or crosses the cylinder center.');
    if(next.Z!==null) {minZ=Math.min(minZ,next.Z);maxZ=Math.max(maxZ,next.Z);}
    if(coords.includes(axial)){minAxial=Math.min(minAxial,next[axial]);maxAxial=Math.max(maxAxial,next[axial]);}
    if(fixture && ((coords.includes(axial)&&(next[axial]<fixture.axial_min-1e-9||next[axial]>fixture.axial_max+1e-9)) || next.Z!==null&&radius+next.Z<fixture.min_radius-1e-9))throw Error('Rotary path exceeds the fixture clearance envelope.');
    if(cutting){lastFeed=number(words.F,'Source feed',.001,100000);if(coords.some(k=>state[k]===null))throw Error('Cutting starts from an unknown coordinate.');}
    const output=[];
    for(const k of coords){let v=words[k];if(k===wrapAxis){const t=Math.round(v/circumference);if(Math.abs(v-t*circumference)<=.00051)v=t*circumference;
        const a=offset+direction*360*v/circumference;if(Math.abs(a)>360000)throw Error('Rotary extent exceeds 1000 revolutions.');min=Math.min(min,a);max=Math.max(max,a);output.push(rotaryAxis+a.toFixed(6));}
      else output.push((k==='Z'?radialOut:axialOut)+v.toFixed(6));
    }
    if(cutting){
      const delta=k=>coords.includes(k)?next[k]-state[k]:0;
      const da=delta(axial),dz=delta('Z'),dw=delta(wrapAxis);
      let length=Math.hypot(da,dz,dw);
      if(reference==='cut'&&dw){
        const rate=Math.abs(dw)/radius, body=Math.hypot(da,dz), a=radius+(state.Z??0), b=radius+(next.Z??0);
        if(Math.abs(dz)<1e-7*Math.max(a,b))length=Math.hypot(body,rate*(a+b)/2);
        else {const integral=r=>(r*Math.hypot(body,rate*r)+(body*body/rate)*Math.asinh(rate*r/body))/2;
          length=(integral(b)-integral(a))/dz;}
      }
      Object.assign(state,next);if(length<=1e-12)continue;
      const inverse=number(lastFeed/length,'Inverse-time feed',1e-9,1e9);
      if(post==='xpro-v5'&&!inverseMode){lines.push('G93');inverseMode=true;}
      lines.push('G1 '+output.join(' ')+' F'+inverse.toFixed(9)+(laser?' S'+words.S.toFixed(0):''));moves++;minutes+=length/lastFeed;
    }else{
      Object.assign(state,next);if(post==='xpro-v5'&&inverseMode){lines.push('G94');inverseMode=false;}
      lines.push('G0 '+output.join(' '));
    }
  }
  if(!Number.isFinite(min)||!Number.isFinite(minAxial)||!Number.isFinite(minZ)||!moves||lastFeed===null||source.at(-1)?.trim()!=='M5')throw Error('Rotary job needs a wrapped cut and final M5.');
  lines.push('G94 F'+lastFeed.toFixed(6));
  const warnings=[...(job.warnings||[]),`${rotaryAxis} rotates about ${axialOut}. Set the axial work zero, angular seam and surface work zero on ${radialOut} for this fixture.`,
    'Continuous angles retain full turns. Check angular travel and clearance of the tool, chuck and holder.'];
  if(post==='linuxcnc')warnings.push('LinuxCNC rotary output is file export only.');
  return {...job,lines,warnings,rotary:{postprocessor:post,export_only:post==='linuxcnc',diameter,circumference,wrap_axis:wrapAxis,output_axis:rotaryAxis,axial_output_axis:axialOut,radial_output_axis:radialOut,feed_reference:reference,angle_offset:offset,direction,angle_bounds:[min,max],axial_bounds:[minAxial,maxAxial],radial_bounds:[minZ,maxZ],feed_mode:'G93',feed_minutes:minutes,fixture:fixture??null,seam_snap_tolerance_mm:.00051,preview_coordinates:'planar'}};
}
function generate(project,options){
  if(!project||!options)throw Error('Supply a project and rotary setup.');
  const protocol=project.protocol??'grbl';
  if(!['grbl','grblhal','xpro-v5','fluidnc','linuxcnc'].includes(protocol)||protocol==='linuxcnc'&&options.postprocessor!=='linuxcnc')throw Error('Choose a compatible source protocol for the rotary postprocessor.');
  const cam=root.CNCOffline || (typeof require==='function'?require('./offline-cam.js'):null);
  return wrap(cam.generate(project),options);
}
const api={wrap,generate};if(typeof module!=='undefined')module.exports=api;else root.CNCRotary=api;
})(typeof window!=='undefined'?window:globalThis);
