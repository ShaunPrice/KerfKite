/* Portable design envelopes. Choosing a profile never configures a controller. */
((root)=>{
  'use strict';
  function validate(raw) {
    if(!raw||typeof raw!=='object'||Array.isArray(raw)||typeof raw.name!=='string'||!raw.name.trim()||raw.name.trim().length>80) throw Error('Enter a machine name of 1–80 characters.');
    const check=(v,lo,hi)=>typeof v==='number'&&Number.isFinite(v)&&v>=lo&&v<=hi;
    for(const key of ['width','height'])if(!check(raw[key],1,2000))throw Error('Enter X and Y travel between 1 and 2,000 mm.');
    if(raw.zTravel!==null&&!check(raw.zTravel,.1,2000))throw Error('Enter measured Z travel in mm or leave it blank.');
    const protocol=CNCProtocols.get(raw.protocol).id;
    // A profile made from a controller's own report remembers which machine it describes.
    const source=typeof raw.source==='string'&&raw.source&&raw.source.length<=120?{source:raw.source}:{};
    return {name:raw.name.trim(),width:raw.width,height:raw.height,zTravel:raw.zTravel,protocol,...source};
  }
  if(typeof module!=='undefined'&&module.exports)module.exports={validate};
  if(typeof document==='undefined')return;
  const panel=document.createElement('details');panel.id='machine-library';
  panel.innerHTML=`<summary>My machine profiles</summary>
    <p class="hint">Store a measured work area and controller dialect. Applying a profile changes the design setup in this project.</p>
    <label>Machine name<input id="machine-name" maxlength="80"></label>
    <div class="pair"><label>X travel · mm<input id="machine-width" type="number" min="1" max="2000" step="any"></label>
    <label>Y travel · mm<input id="machine-height" type="number" min="1" max="2000" step="any"></label>
    <label>Z travel · mm (optional)<input id="machine-z" type="number" min=".1" max="2000" step="any"></label></div>
    <label>Design dialect<select id="machine-dialect"></select></label>
    <div class="buttons"><button id="machine-save">Save as new profile</button><button id="machine-update">Update selected profile</button>
    <button id="machine-read">Set up from controller</button><button id="machine-export">Export profile</button><button id="machine-import">Import profile</button><button id="machine-forget">Remove from library</button></div>
    <input id="machine-file" type="file" accept=".json" hidden><p id="machine-info" role="status"></p>`;
  $('profileHint').after(panel);
  const el=id=>$('machine-'+id), select=$('profile');
  el('dialect').replaceChildren(...CNCProtocols.profiles.map(p=>new Option(p.name,p.id)));
  let storageKey, profiles=[],projectCopy=null,defaultSelect;
  const current=()=>profiles.find(p=>p.id===select.value)||((projectCopy?.id===select.value)?projectCopy:null);
  function fields(p) {
    el('name').value=p?.name||'';el('width').value=p?.width??$('width').value;
    el('height').value=p?.height??$('height').value;el('z').value=p?.zTravel??'';
    el('dialect').value=p?.protocol||$('protocol').value;
    const local=profiles.some(p=>p.id===select.value);
    el('update').disabled=el('forget').disabled=!local;
  }
  function options() {
    for(const opt of Array.from(select.options))if(opt.dataset.custom)opt.remove();
    for(const p of [...profiles,...(projectCopy&&!profiles.some(item=>item.id===projectCopy.id)?[projectCopy]:[])]){
      const opt=new Option(p.name+(p===projectCopy?' · project copy':''),p.id);
      opt.dataset.custom='1';select.append(opt);
    }
  }
  function read() {
    return validate({name:el('name').value,width:el('width').valueAsNumber,height:el('height').valueAsNumber,
      zTravel:el('z').value.trim()?el('z').valueAsNumber:null,protocol:el('dialect').value});
  }
  function store(items) {
    if(!storageKey)throw Error('Machine library is not ready.');
    if(items.length>64)throw Error('The library holds up to 64 profiles.');
    localStorage.setItem(storageKey,JSON.stringify(items));profiles=items;
  }
  function checkBed(p) {
    for(const obj of objects){const [x,y,r,t]=bounds(obj.paths);
      if(x<0||y<0||r>p.width||t>p.height)throw Error('This work area excludes existing artwork. Move the artwork or start a new project first.');}
  }
  function apply(p){
    $('width').value=p.width;$('height').value=p.height;$('protocol').value=p.protocol;
    $('protocol').dispatchEvent(new Event('change',{bubbles:true}));
    window.projectExtras.machineProfile=structuredClone(p);
    $('profileHint').textContent=p.name+' · '+p.width+' × '+p.height+' mm'+(p.zTravel===null?'':' · Z travel '+p.zTravel+' mm');
    fields(p);invalidate();draw();window.dispatchEvent(new Event('cnc-project-changed'));
  }
  function save(isUpdate){
    const raw=read();checkBed(raw);
    const id=isUpdate?select.value:'user-'+crypto.randomUUID(),item={id,...raw};
    if(isUpdate&&!profiles.some(p=>p.id===id))throw Error('Select a local profile to update.');
    store(isUpdate?profiles.map(p=>p.id===id?item:p):[...profiles,item]);projectCopy=null;options();select.value=id;
    apply(item);previousId=id;el('info').textContent='Profile saved and applied to this project.';
  }
  // Set the workspace from what the connected controller reports about its machine (/api/machine).
  // The report is read, never written: the controller's own settings are left alone.
  function adopt(d){
    const env=d.environment||{},done=[],text=value=>String(Math.round(value*1000)/1000);
    if(env.width&&env.height){
      let item=profiles.find(p=>p.source===d.identity);
      const known=!!item;
      if(!item)item={id:'user-'+crypto.randomUUID(),...validate({name:d.name,width:env.width,height:env.height,zTravel:env.z_travel??null,protocol:env.protocol,source:d.identity})};
      // A machine this project is already set up for keeps whatever was changed since.
      if(!known||select.value!==item.id){
        checkBed(item);
        if(!known)store([...profiles,item]);
        projectCopy=null;options();select.value=item.id;apply(item);previousId=item.id;
      }
      done.push((known?'Recognised ':'New machine: ')+item.name+(item.zTravel===null?'':', Z travel '+text(item.zTravel)+' mm'));
    }
    for(const [id,label,limit] of [['power','Speed or power',env.power_max],['feed','Feed',env.feed_max],['plunge','Plunge feed',env.plunge_max]]){
      const input=$(id);if(!limit||!input)continue;
      input.max=limit;
      if(Number(input.value)>limit){input.value=text(limit);input.dispatchEvent(new Event('change',{bubbles:true}));done.push(label+' lowered to the controller limit of '+text(limit));}
    }
    if(typeof env.laser_mode==='boolean'&&env.laser_mode!==($('mode').value==='laser')){
      $('mode').value=env.laser_mode?'laser':'router';$('mode').dispatchEvent(new Event('change',{bubbles:true}));
      done.push('Head set to '+(env.laser_mode?'laser':'router')+' to match the controller');
    }
    const summary=[done.length?done.join('. ')+'.':'The controller reported no workspace size.',
      d.reported.length?'Reported: '+d.reported.join(', ')+'.':'',
      d.missing.length?'Not reported, set by hand: '+d.missing.join(', ')+'.':'',...d.warnings.map(w=>w+'.')].filter(Boolean).join(' ');
    el('info').textContent=summary;return summary;
  }
  async function readMachine(asked){
    const d=await api('machine');
    if(!d.available){if(asked)throw Error(d.reason);return d.waiting;}
    message(adopt(d));return false;
  }
  el('read').onclick=task(()=>readMachine(true));
  // The first time a controller is connected and has reported its settings, the workspace follows it.
  let handled=false,reading=false,settle=0;
  // Each Connect is a new machine until it has answered; the pause lets the request reach the server first.
  $('connect').addEventListener('click',()=>{handled=false;settle=Date.now()+1500;});
  if(!offline)setInterval(async()=>{
    if(!latest?.connected){handled=false;return;}
    if(handled||reading||Date.now()<settle||latest.simulated||!window.studioSession?.machine_readback)return;
    reading=true;
    try{handled=!(await readMachine(false));}catch(error){handled=true;message(error.message,true);}finally{reading=false;}
  },1000);
  el('save').onclick=task(()=>save(false));el('update').onclick=task(()=>save(true));
  el('export').onclick=task(()=>download('machine-profile.json',JSON.stringify({format:'kerfkite-machine/1',profile:read()},null,2),'application/json'));
  el('import').onclick=()=>el('file').click();
  el('file').onchange=task(async()=>{
    const file=el('file').files[0];if(!file)return;el('file').value='';
    if(file.size>16384)throw Error('Machine profile exceeds 16 KB.');
    const data=JSON.parse(await file.text());if(data?.format!=='kerfkite-machine/1')throw Error('Choose a KerfKite machine profile.');
    fields(validate(data.profile));el('update').disabled=true;
    el('info').textContent='Imported for review. Save as new profile to apply.';
  });
  el('forget').onclick=task(()=>{
    const p=current();if(!p||!confirm('Remove this profile from this browser library? The project keeps its copy.'))return;
    store(profiles.filter(row=>row.id!==p.id));projectCopy=p;options();select.value=p.id;fields(p);
  });
  function restore() {
    const raw=window.projectExtras?.machineProfile;
    projectCopy=null;options();
    if(raw){
      try{
        const rawId=String(raw.id||'');
        const p=validate(raw);
        const existing=profiles.find(item=>item.id===rawId&&JSON.stringify(validate(item))===JSON.stringify(p));
        projectCopy=existing?null:{id:/^user-[\da-f-]{36}$/.test(rawId)&&!profiles.some(item=>item.id===rawId)?rawId:'project-copy',...p};
        options();select.value=(existing||projectCopy).id;
        $('profileHint').textContent='Saved design profile · '+p.name;fields(p);
      }catch{fields(null);el('info').textContent='The saved machine profile could not be read.';}
    }else{
      if(select.options.length)select.selectedIndex=0;
      fields(null);$('profileHint').textContent='Current project work area · '+$('width').value+' × '+$('height').value+' mm';
    }
  }
  let previousId='';
  root.studioReady.then(session=>{
    storageKey='kerfkite-machines/1/'+(session.web_mode?'account/'+session.username:'local');
    try{
      const items=JSON.parse(localStorage.getItem(storageKey)||'[]');
      if(!Array.isArray(items)||items.length>64)throw Error();
      profiles=items.map(item=>{if(!/^user-[\da-f-]{36}$/.test(item?.id))throw Error();return {id:item.id,...validate(item)};});
    }catch{el('info').textContent='This browser could not load the machine library.';}
    defaultSelect=select.onchange;options();fields(null);previousId=select.value;
    select.onchange=task(()=>{
      const p=current();
      if(p){try{checkBed(p);}catch(error){select.value=previousId;throw error;}apply(p);}
      else{delete window.projectExtras.machineProfile;defaultSelect();fields(null);}
      previousId=select.value;
    });
    if(window.projectExtras.machineProfile){restore();previousId=select.value;}
  });
  root.addEventListener('cnc-project-loaded',()=>{restore();previousId=select.value;});
})(globalThis);
