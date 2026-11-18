/* Quote the XYZ program emitted by Studio. All distances are mm, times minutes. */
((root)=>{
  'use strict';
  function timing(lines, setup, protocol, dwellUnits) {
    const rapid=setup.rapid, position=setup.start.slice();
    if(rapid.length!==3 || rapid.some(v=>!Number.isFinite(v)||v<=0||v>100000) ||
       position.length!==3 || position.some(v=>!Number.isFinite(v)||Math.abs(v)>10000))throw Error('Enter three valid axis rapid rates and a starting position.');
    if(['mach3','uccnc'].includes(protocol)&&!['seconds','milliseconds'].includes(dwellUnits))throw Error('Select the dwell units configured on this controller.');
    let metric=false,absolute=false,feed=0,cut=0,travel=0,dwell=0;
    const ms=protocol==='marlin'||(['mach3','uccnc'].includes(protocol)&&dwellUnits==='milliseconds');
    for(const [i,line] of lines.entries()){
      const [command,...words]=line.trim().split(/\s+/);
      if(!command)continue;
      if(command==='G21'){metric=true;continue;}
      if(command==='G90'){absolute=true;continue;}
      if(['G17','G94','G54','G40','G49','G80','G61','M3','M4','M5','M30'].includes(command))continue;
      if(!['G0','G1','G4'].includes(command))throw Error('Cycle estimate cannot interpret '+command+' on line '+(i+1));
      const values={};
      for(const word of words){
        const key=word[0],v=Number(word.slice(1));
        if(!/^[XYZFPS]-?(?:\d+\.?\d*|\.\d+)$/.test(word)||!Number.isFinite(v)||Object.hasOwn(values,key))throw Error('Invalid program word on line '+(i+1));
        values[key]=v;
      }
      if(command==='G4'){
        if(!Number.isFinite(values.P)||values.P<0)throw Error('Invalid dwell on line '+(i+1));
        dwell+=values.P/(ms?60000:60);continue;
      }
      if(!metric||!absolute)throw Error('Cycle time requires absolute millimetre coordinates.');
      const delta=['X','Y','Z'].map((key,k)=>(values[key]??position[k])-position[k]);
      if(values.F!==undefined)feed=values.F;
      if(command==='G0')travel+=Math.max(...delta.map((d,k)=>Math.abs(d)/rapid[k]));
      else{
        if(feed<=0)throw Error('Missing feed rate on line '+(i+1));
        cut+=Math.hypot(...delta)/feed;
      }
      delta.forEach((d,k)=>{position[k]+=d;});
    }
    return {cut,travel,dwell};
  }
  function calibrated(result, actual) {
    const motion=result.cut+result.travel;
    if(!Number.isFinite(actual)||actual<=result.dwell||motion<=0)throw Error('Enter the measured cycle duration, including dwell, for this preview.');
    return (actual-result.dwell)/motion;
  }
  root.KerfCycleTime={timing,calibrated};
  if(typeof module!=='undefined'&&module.exports)module.exports=root.KerfCycleTime;
  if(typeof document==='undefined')return;
  const panel=document.createElement('details');panel.id='cyclePanel';
  panel.innerHTML=`<summary>Machine cycle time</summary>
    <p class="hint">Set the machine’s rapid rates and tool-tip start position. A measured sample can account for acceleration and controller overhead.</p>
    <fieldset><legend>Axis rapid rates · mm/min</legend><div class="pair" id="time-rapids"></div></fieldset>
    <fieldset><legend>Starting position · work coordinates, mm</legend><div class="pair" id="time-starts"></div></fieldset>
    <button id="time-estimate">Estimate current preview</button>
    <label>Measured duration of this preview · minutes<input id="time-measured" type="number" min=".001" step="any"></label>
    <div class="buttons"><button id="time-calibrate">Calibrate from sample</button><button id="time-reset">Clear calibration</button></div>
    <p id="time-result" role="status">Generate a preview before estimating its cycle.</p>
    <button id="time-quote">Use cycle time in quote</button>`;
  $('machine-panel-quote').prepend(panel);
  const axes=['X','Y','Z'],defaults={rapid:[2500,2500,400],start:[0,0,0]};
  for(const key of ['rapid','start'])axes.forEach((axis,k)=>{
    const label=document.createElement('label');label.textContent=axis;
    const input=document.createElement('input');input.type='number';input.step='any';
    input.id='time-'+key+axis;input.value=defaults[key][k];label.append(input);$('time-'+key+'s').append(label);
  });
  const read=key=>axes.map(a=>$('time-'+key+a).valueAsNumber);
  function current() {
    const p=machiningProject();
    if(!preview||previewProject!==JSON.stringify(p))throw Error('Generate a current toolpath preview first.');
    return timing(preview.lines,{rapid:read('rapid'),start:read('start')},p.protocol,p.dwell_units);
  }
  function matching() {
    const c=window.projectExtras?.cycleTime?.sample;
    return c?.mode===$('mode').value&&c?.protocol===$('protocol').value&&
      c?.profile===$('profile').value&&Array.isArray(c.rapid)&&c.rapid.length===3&&c.rapid.every((v,k)=>v===read('rapid')[k])&&Number.isFinite(c.factor)&&c.factor>0?c:null;
  }
  function persist(sample=window.projectExtras?.cycleTime?.sample) {
    window.projectExtras.cycleTime={start:read('start'),rapid:read('rapid'),sample:sample||null};
    window.dispatchEvent(new Event('cnc-project-changed'));
  }
  function show(){
    const result=current(),sample=matching(),factor=sample?.factor??1;
    const total=(result.cut+result.travel)*factor+result.dwell;
    $('time-result').textContent=total.toFixed(2)+' min estimated · programmed feed '+result.cut.toFixed(2)+
      ' min · rapid '+result.travel.toFixed(2)+' min · dwell '+result.dwell.toFixed(2)+' min · '+
      (sample?'measured motion factor '+factor.toFixed(3):'nominal rates, uncalibrated');
    return total;
  }
  $('time-estimate').onclick=task(show);
  $('time-calibrate').onclick=task(()=>{
    const result=current(),minutes=$('time-measured').valueAsNumber,factor=calibrated(result,minutes);
    persist({factor,minutes,...result,profile:$('profile').value,mode:$('mode').value,protocol:$('protocol').value,rapid:read('rapid')});show();
  });
  $('time-reset').onclick=task(()=>{persist(null);$('time-result').textContent='Calibration cleared.';});
  $('time-quote').onclick=task(()=>{
    const total=show();$('cost_minutes').value=Number(total.toFixed(5));$('cost_timeFactor').value=1;
    $('cost_minutes').dispatchEvent(new Event('input',{bubbles:true}));
    message('Cycle time copied to the quote. The cut-time multiplier is now 1; your extra allowance is retained.');
  });
  panel.addEventListener('change',event=>{
    if(event.target.id==='time-measured')return;
    if(read('rapid').every(v=>Number.isFinite(v)&&v>0)&&read('start').every(Number.isFinite))persist();
    $('time-result').textContent='Settings changed. Estimate the current preview again.';
  });
  window.addEventListener('cnc-project-loaded',()=>{
    const s=window.projectExtras?.cycleTime;
    for(const key of ['rapid','start'])axes.forEach((a,k)=>{$('time-'+key+a).value=Number.isFinite(s?.[key]?.[k])?s[key][k]:defaults[key][k];});
    $('time-measured').value='';$('time-result').textContent='Generate a preview before estimating its cycle.';
  });
})(globalThis);
