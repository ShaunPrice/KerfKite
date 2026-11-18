// Preview conversions in a draft before adding anything to the open document.
(() => {
  'use strict';
  const extensions = 'lbrn lbrn2 lbdev lbprefs lbzip clb lbset settings dxf nc ngc cnc tap gcode plt hpgl hpg ini'.split(' ');
  const program = /\.(dxf|nc|ngc|cnc|tap|gcode|plt|hpgl|hpg|ini)$/i;
  const ext = name => name.split('.').pop().toLowerCase();
  const panel=document.createElement('section');
  panel.className='solid-group';
  panel.innerHTML=`
    <h3>Import artwork and settings</h3>
    <label>Source file<input id="exchange-source-file" type="file"></label>
    <div class="pair"><label>Laser S at 100%<input type="number" min="1" max="100000" value="1000" id="import-max-s"></label>
    <label>Unitless DXF<select id="import-unit"><option value="mm">Millimetres</option><option value="in">Inches</option></select></label></div>
    <div id="import-bundle" hidden><label>Bundle member<select id="import-member"></select></label><button id="import-read-member">Review selected member</button></div>
    <button id="import-inspect">Review import</button> <button id="import-example">Try a sample</button>
    <div id="import-review" hidden><p id="import-summary"></p><svg id="import-drawing" role="img" aria-label="Drawing to import" style="width:100%;height:130px;background:#f2f6f6"></svg>
    <details><summary>Conversion details</summary><ul id="import-notes"></ul></details>
    <button id="import-commit">Add to project</button></div>
    <p id="import-status" role="status"></p>
    <div id="import-shelf" hidden><h3>Imported setup library</h3><label>Setup<select id="import-setup"></select></label>
    <p id="import-setup-info"></p><button id="import-use">Apply to workspace</button></div>`;
  $('design-panel-projects').prepend(panel);
  const input=$('exchange-source-file');
  input.accept=extensions.map(s=>'.'+s).join(',');
  const status=s=>{$('import-status').textContent=s;};
  const shelf=()=>window.projectExtras?.importSetups||[];
  function describe(item){
    if(!item)return '';
    if(item.kind==='device')return item.width+' × '+item.height+' mm work area';
    return [item.mode,...Object.entries(item.settings).map(([k,v])=>k+' '+v)].filter(Boolean).join(' · ');
  }
  function showShelf(){
    const list=shelf(),select=$('import-setup');
    $('import-shelf').hidden=!list.length;
    select.replaceChildren(...list.map((p,i)=>new Option(p.name,String(i))));
    $('import-setup-info').textContent=describe(list[select.selectedIndex]);
  }
  $('import-setup').onchange=()=>{$('import-setup-info').textContent=describe(shelf()[$('import-setup').selectedIndex]);};
  $('import-use').onclick=task(()=>{
    const item=shelf()[$('import-setup').selectedIndex];if(!item)throw Error('Select a setup');
    if(item.kind==='device'){
      const w=Number(item.width),h=Number(item.height);
      if(!(w>=1&&w<=2000&&h>=1&&h<=2000))throw Error('Invalid work area');
      $('width').value=w;$('height').value=h;
    }else{
      const keys=['feed','power','passes','operation','interval','hatch_angle','crosshatch','knife_offset','depth','step','plunge'];
      for(const k of keys)if(item.settings[k]!=null)$(k).value=item.settings[k];
      if(['laser','router','knife'].includes(item.mode))$('mode').value=item.mode;
    }
    modeUI();invalidate();draw();status('Workspace updated. Review CAM and generate a new preview.');
  });
  let draft, bundle, revision=0;
  function clear(){revision++;draft=null;bundle=null;$('import-bundle').hidden=true;$('import-review').hidden=true;status('');}
  function preview(paths){
    const svg=$('import-drawing');svg.replaceChildren();svg.style.display=paths.length?'':'none';
    if(!paths.length)return;
    let x=Infinity,y=Infinity,X=-Infinity,Y=-Infinity;
    for(const o of paths){
      const el=document.createElementNS('http://www.w3.org/2000/svg','path');
      el.setAttribute('d',o.paths.map(p=>p.map(([a,b],i)=>{x=Math.min(x,a);X=Math.max(X,a);y=Math.min(y,b);Y=Math.max(Y,b);return (i?'L':'M')+a+' '+(-b);}).join(' ')).join(' '));
      el.setAttribute('stroke','#176a79');el.setAttribute('fill','none');el.setAttribute('vector-effect','non-scaling-stroke');svg.append(el);
    }
    const margin=Math.max(1,(X-x+Y-y)*.03);
    svg.setAttribute('viewBox',[x-margin,-Y-margin,X-x+2*margin,Y-y+2*margin].join(' '));
  }
  async function inspect(file, member=false){
    if(member){revision++;draft=null;$('import-review').hidden=true;}else clear();
    const id=revision;
    if(!file)throw Error('Choose a project, settings file or cutter program');
    if(file.size>4e6)throw Error('Import files up to 4 MB');
    if(ext(file.name)==='lbzip'){
      const reader=new FileReader();
      const content=await new Promise((ok,fail)=>{reader.onload=()=>ok(reader.result.split(',')[1]);reader.onerror=()=>fail(Error('Cannot read bundle'));reader.readAsDataURL(file);});
      const list=await api('import-bundle',{content});if(id!==revision)return;
      if(!list.files.length)throw Error('No supported files in this bundle');
      bundle=content;$('import-member').replaceChildren(...list.files.map(row=>new Option(row.name,String(row.index))));
      $('import-bundle').hidden=false;status(list.files.length+' supported members · '+list.skipped+' skipped');return;
    }
    const text=await file.text(),name=file.name;
    const r=program.test(name)?await api('program-import',{name:ext(name)==='dxf'?'drawing.dxf':'program.'+ext(name),text,unit:$('import-unit').value}):
      CNCLightBurn.read(name,text,{maxPower:$('import-max-s').valueAsNumber});
    if(id!==revision)return;
    const needed=new Set([...r.layers.map(l=>String(l.key)),...r.objects.map(o=>String(o.sourceLayer??''))]);
    if(CNCDesignLayers.layers().length+needed.size>64)throw Error('Adding these layers would exceed the 64-layer limit. Open a new project or import fewer layers.');
    preview(r.objects);
    const setupCount=r.devices.length+r.presets.length;
    $('import-summary').textContent=name+' · '+r.objects.length+' objects · '+r.layers.length+' layers · '+setupCount+' setups';
    const notes=[...r.warnings];
    if(r.objects.length)notes.unshift('New layers start with Output off. Review scale, orientation, head and settings in Layers before enabling output.');
    $('import-notes').replaceChildren(...notes.map(s=>{const li=document.createElement('li');li.textContent=s;return li;}));
    draft=r;$('import-review').hidden=false;status('Review ready.');
  }
  $('import-read-member').onclick=task(async()=>{
    const content=bundle,id=++revision;draft=null;$('import-review').hidden=true;
    if(!content)throw Error('Open a bundle first');
    const result=await api('import-bundle',{content,index:Number($('import-member').value)});
    if(id===revision)await inspect(new File([result.text],result.name),true);
  });
  $('import-member').onchange=()=>{draft=null;revision++;$('import-review').hidden=true;};
  $('import-inspect').onclick=task(()=>inspect(input.files[0]));
  $('import-example').onclick=task(()=>inspect(new File(['<LightBurnProject><CutSetting type="Cut"><index Value="1"/><name Value="Outline"/><speed Value="8"/><maxPower Value="35"/></CutSetting><Shape Type="Rect" CutIndex="1" W="30" H="18" Cr="3"><XForm>1 0 0 1 30 24</XForm></Shape></LightBurnProject>'],'Practice badge.lbrn2')));
  for(const id of ['import-max-s','import-unit'])$(id).oninput=clear;
  input.onchange=task(()=>inspect(input.files[0]));
  $('import-commit').onclick=task(()=>{
    const r=draft;if(!r)throw Error('Review a source file first');
    const rows=CNCDesignLayers.layers(), map=new Map(), linked=new Set(r.objects.map(o=>String(o.sourceLayer??'')));
    const definitions=r.layers.map(l=>({...l,key:String(l.key)}));
    for(const key of linked)if(!definitions.some(l=>l.key===key))definitions.push({key,name:key||'Imported artwork'});
    const wasEmpty=!objects.length;
    for(const l of definitions){
      const id='import_'+crypto.randomUUID().replaceAll('-','');map.set(l.key,id);
      rows.push({id,name:String(l.name||'Imported layer').slice(0,80),color:l.color||'#176a79',
        visible:l.visible!==false,output:false,settings:l.settings||{}});
    }
    const valid=CNCDesignLayers.catalog(rows);
    const additions=r.objects.map(o=>{const {sourceLayer,...data}=o;const original=definitions.find(l=>l.key===String(sourceLayer??''));
      return {...data,designLayerId:map.get(String(sourceLayer??'')),...(original?.output===false?{output:false}:{})};});
    const combined=objects.concat(additions),paths=combined.flatMap(o=>o.paths);
    if(combined.length>1000||paths.length>3000||paths.reduce((n,p)=>n+p.length,0)>100000||paths.some(p=>p.some(pt=>pt.some(v=>!Number.isFinite(v)||Math.abs(v)>2000))))throw Error('Imported drawing exceeds Studio project limits');
    const setups=[...shelf(),...r.devices.map(d=>({...d,kind:'device'})),...r.presets.map(p=>({...p,mode:p.mode||r.mode,kind:'preset'}))];
    if(setups.length>4000)throw Error('Project setup library exceeds 4,000 entries');
    const extra={...window.projectExtras,importSetups:setups,designLayers:valid};
    const prospective={...CNCProjects.document(),objects:combined,extras:extra};
    if(new TextEncoder().encode(JSON.stringify(prospective,null,2)).length>4e6)throw Error('Import exceeds the portable project size limit');
    if(wasEmpty&&r.objects.length&&['laser','router','knife'].includes(r.mode))$('mode').value=r.mode;
    window.projectExtras=extra;objects=combined;if(additions.length)selected=combined.length-1;
    modeUI();invalidate();draw();window.dispatchEvent(new Event('cnc-layers-changed'));window.dispatchEvent(new Event('cnc-project-changed'));
    clear();showShelf();status('Added '+additions.length+' objects and '+(r.devices.length+r.presets.length)+' setups.');
  });
  window.CNCInterchange={recognises:file=>extensions.includes(ext(file.name)),open:async file=>{$('design-tab-projects').click();await inspect(file);}};
  window.addEventListener('cnc-project-loaded',()=>{clear();showShelf();});
  showShelf();
})();
