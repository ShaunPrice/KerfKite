/* Portable LinuxCNC cylindrical export. Never sends machine commands. */
(function(root){
'use strict';
const fail=message=>{throw Error(message);};
function number(v,label,lo,hi){if(typeof v!=='number'||!Number.isFinite(v)||v<lo||v>hi)fail(`${label} must be a finite number in ${lo}..${hi}`);return v;}
function wrap(job,options){
 if(!job||typeof job!=='object'||!options||typeof options!=='object')fail('Rotary job and options must be objects');
 if(job.mode!=='router')fail('Rotary export supports router jobs only');
 if(options.postprocessor!=='linuxcnc')fail('Select the LinuxCNC rotary export postprocessor');
 const axis=options.wrap_axis;if(!['X','Y'].includes(axis))fail('wrap_axis must explicitly be X or Y');
 const diameter=number(options.diameter,'Diameter',1,2000),offset=number(options.angle_offset??0,'Angle offset',-360000,360000),direction=number(options.direction??1,'Direction',-1,1);
 if(![-1,1].includes(direction))fail('Direction must be -1 or 1');
 let source=job.lines;if(!Array.isArray(source)||source.length<1||source.length>200000||source.some(l=>typeof l!=='string'||l.length>256))fail('Invalid or oversized rotary source');
 if(source.at(-1)==='M30')source=source.slice(0,-1);
 const circumference=Math.PI*diameter,scale=direction*360/circumference,state={X:null,Y:null,Z:null},output=[];
 let units=false,absolute=false,feedMode=false,lastFeed=null,moves=0,angleMin=Infinity,angleMax=-Infinity;
 for(const raw of source){
  const tokens=raw.trim().split(/\s+/),words={};
  for(const token of tokens){const m=/^([A-Z])([+-]?(?:\d+(?:\.\d*)?|\.\d+))$/.exec(token);if(!m||Object.hasOwn(words,m[1]))fail('Rotary requires canonical generated G-code without comments or duplicate words');words[m[1]]=number(Number(m[2]),'Source word',-1e9,1e9);}
  const command=tokens[0],keys=Object.keys(words);
  if(['G21','G90','G17','G54','G94','M5','G40','G49','G80','G61'].includes(command)&&tokens.length===1){if(command==='G21')units=true;if(command==='G90')absolute=true;if(command==='G94')feedMode=true;output.push(command==='G94'?'G93':command);continue;}
  if(command==='M3'&&keys.length===2&&Object.hasOwn(words,'S')&&words.S>0){output.push(raw.trim());continue;}
  if(command==='G4'&&keys.length===2&&Object.hasOwn(words,'P')&&words.P>=0&&words.P<=60){output.push(raw.trim());continue;}
  if(!['G0','G1'].includes(command))fail('Unsupported rotary source command');
  if(!units||!absolute||!feedMode)fail('Rotary source must establish G21 G90 G94 before motion');
  if(keys.some(k=>!['G','X','Y','Z',...(command==='G1'?['F']:[])].includes(k))||!['X','Y','Z'].some(k=>Object.hasOwn(words,k)))fail('Unsupported words in rotary motion');
  if(Object.hasOwn(words,'Z')&&words.Z<=-diameter/2)fail('Cut depth reaches or crosses the cylinder center');
  const coords=['X','Y','Z'].filter(k=>Object.hasOwn(words,k));let delta=0;
  if(command==='G1'){lastFeed=number(words.F,'Source linear feed',.001,100000);for(const k of coords){if(state[k]===null)fail('Cutting move starts from an unknown position');delta+=(words[k]-state[k])**2;}}
  const transformed=[];
  for(const k of coords){let destination=words[k];if(k===axis){const turn=Math.round(destination/circumference);if(Math.abs(destination-turn*circumference)<=.00051)destination=turn*circumference;const angle=offset+destination*scale;if(Math.abs(angle)>360000)fail('Rotary extent exceeds 1000 revolutions');angleMin=Math.min(angleMin,angle);angleMax=Math.max(angleMax,angle);transformed.push('A'+angle.toFixed(6));}else transformed.push((k==='Z'?'Z':'X')+destination.toFixed(6));state[k]=words[k];}
  if(command==='G1'){const length=Math.sqrt(delta);if(length<=1e-12)continue;const inverse=number(lastFeed/length,'Inverse-time feed',1e-9,1e9);output.push('G1 '+transformed.join(' ')+' F'+inverse.toFixed(9));moves++;}else output.push('G0 '+transformed.join(' '));
 }
 if(!Number.isFinite(angleMin)||!moves||lastFeed===null)fail('Rotary job needs wrapped-axis motion and a nonzero cutting move');
 if(source.at(-1).trim()!=='M5')fail('Rotary source must end with spindle off (M5)');
 output.push('G94 F'+lastFeed.toFixed(6));
 return {...job,lines:output,warnings:[...(job.warnings||[]),'LinuxCNC rotary export only; current GRBL/Marlin senders cannot run A-axis files.','Configure continuous A rotation around X, radial surface Z zero, accurate diameter and seam alignment.','Preview remains planar; fixture, chuck and holder collision checks are not included.','G93 uses nominal unwrapped surface timing. Acceleration and reduced-radius speed still require commissioning.'],rotary:{postprocessor:'linuxcnc',export_only:true,diameter,circumference,wrap_axis:axis,output_axis:'A',angle_offset:offset,direction,angle_bounds:[angleMin,angleMax],feed_mode:'G93',axial_output_axis:'X',seam_snap_tolerance_mm:.00051,preview_coordinates:'planar'}};
}
const api={wrap};if(typeof module!=='undefined')module.exports=api;else root.CNCRotary=api;
})(typeof window!=='undefined'?window:globalThis);
