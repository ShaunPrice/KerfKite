'use strict';
(()=>{
 const left=document.querySelector('main>aside:not(.operations)'),right=document.querySelector('main>aside.operations');
 const leftNodes=[...left.children],rightNodes=[...right.children];
 function tabs(host,prefix,names){
  host.replaceChildren();host.classList.add('tabbed-tools');const nav=document.createElement('div');nav.className='tool-tablist';nav.setAttribute('role','tablist');nav.setAttribute('aria-label',prefix==='design'?'Design tools':'Machining and machine tools');
  const body=document.createElement('div');body.className='tool-panels';host.append(nav,body);const map={};
  names.forEach(([key,title],index)=>{const button=document.createElement('button'),panel=document.createElement('section');button.id=`${prefix}-tab-${key}`;button.type='button';button.textContent=title;button.setAttribute('role','tab');button.setAttribute('aria-controls',`${prefix}-panel-${key}`);panel.id=`${prefix}-panel-${key}`;panel.className='tool-panel';panel.setAttribute('role','tabpanel');panel.setAttribute('aria-labelledby',button.id);panel.tabIndex=0;nav.append(button);body.append(panel);map[key]=panel;
   const activate=()=>{for(const b of nav.children){const active=b===button;b.setAttribute('aria-selected',String(active));b.tabIndex=active?0:-1;document.getElementById(b.getAttribute('aria-controls')).hidden=!active;}body.scrollTop=0;window.dispatchEvent(new Event('cnc-tab-changed'));};button.onclick=activate;
   button.onkeydown=e=>{if(!['ArrowLeft','ArrowRight','Home','End'].includes(e.key))return;e.preventDefault();e.stopPropagation();const bs=[...nav.children],i=bs.indexOf(button),next=e.key==='Home'?0:e.key==='End'?bs.length-1:(i+(e.key==='ArrowRight'?1:-1)+bs.length)%bs.length;bs[next].click();bs[next].focus();};
   button.setAttribute('aria-selected',String(index===0));button.tabIndex=index===0?0:-1;panel.hidden=index!==0;
  });return map;
 }
 const l=tabs(left,'design',[['shapes','Design'],['solid','3D CAD'],['properties','Properties'],['layers','Layers'],['text','Text'],['images','Images'],['templates','Templates'],['projects','Projects'],['objects','Objects']]);
 const r=tabs(right,'machine',[['cam','CAM'],['relief','3D relief'],['rotary','Rotary'],['control','Control'],['quote','Quote'],['help','Help']]);
 leftNodes.filter(n=>n.tagName!=='H2').forEach(n=>l.shapes.append(n));rightNodes.filter(n=>n.tagName!=='H2').forEach(n=>r.cam.append(n));
 function move(id,to,selector){const el=document.getElementById(id);if(el)to.append(selector?el.closest(selector):el);}
 function detail(id,to){move(id,to,'details');}
 move('designLayersPanel',l.layers);move('objectProperties',l.properties);detail('textValue',l.text);detail('rasterFile',l.images);detail('traceImage',l.images);detail('reliefFile',r.relief);detail('assignOperation',r.cam);move('toolChangesPanel',r.cam);move('holdingTabsPanel',r.cam);move('businessPanel',r.quote);move('rotaryPanel',r.rotary);move('pendantPanel',r.control);move('machinePanel',r.control);move('profile',r.control,'label');move('profileHint',r.control);move('projectManager',l.projects);move('fileTransfer',l.projects);
 const projectButtons=document.createElement('div');projectButtons.className='buttons';l.projects.append(projectButtons);for(const id of ['loadProject','saveProject','saveSVG','demo'])move(id,projectButtons);
 const templateButtons=document.createElement('div');templateButtons.className='buttons';l.templates.append(templateButtons);for(const id of ['openLibrary','saveTemplate'])move(id,templateButtons);
 const templateHint=document.createElement('p');templateHint.className='hint';templateHint.textContent='Browse the template library, insert artwork, or save your selection as a reusable template.';l.templates.append(templateHint);
 move('svgImport',l.images);move('file',l.projects);
 move('objects',l.objects);move('move',l.objects,'.buttons');
 // Keep shape icons at the first visible position, independent of other tools.
 const icons=document.getElementById('rect').closest('.buttons');l.shapes.prepend(icons);icons.classList.add('quick-shapes');
 for(const h of l.shapes.querySelectorAll(':scope>h3'))if(h.textContent==='Objects'||h.textContent==='Add geometry')h.remove();
 for(const group of [l,r])for(const panel of Object.values(group)){for(const empty of panel.querySelectorAll(':scope>.buttons'))if(!empty.children.length)empty.remove();for(const d of panel.querySelectorAll(':scope>details')){d.open=true;d.classList.add('tab-section');}}
 r.help.innerHTML='<h2>Help &amp; installation</h2><p class="hint">The complete user guide, installation instructions and fault-finding guide are included with this app.</p><a href="./help.html" target="_blank" rel="noopener">Open the help centre</a><p><a href="./machining-lessons.html" target="_blank" rel="noopener">Interactive machining lessons</a></p>';

 function addMaterialsGuide(panel, id, context) {
  const link = document.createElement('a');
  link.id = id;
  link.textContent = 'Materials guide';
  link.target = '_blank';
  link.rel = 'noopener';
  link.title = 'Look up stock for this tool (opens in a new tab)';
  function updateLink() {
   const mode = document.getElementById('mode').value;
   const camOperation = document.getElementById('operation').value;
   let tool = document.getElementById('manualToolShape')?.value || 'flat';
   let operation = '';
   if (context === 'relief') {
    tool = document.getElementById('reliefTool')?.value || 'flat';
    operation = 'relief';
   } else if (mode === 'laser') {
    tool = 'diode';
    if (camOperation === 'fill') operation = 'engrave';
   } else if (mode === 'knife') {
    tool = 'knife';
   } else if (camOperation === 'vcarve') {
    tool = 'v';
    operation = 'engrave';
   } else if (tool === 'flat' && camOperation === 'line') {
    operation = 'engrave';
   } else if (tool === 'flat' && ['inside','outside','pocket'].includes(camOperation)) {
    operation = camOperation === 'pocket' ? 'pocket' : 'profile';
   }
   const url = new URL('./materials-guide.html', location.href);
   url.searchParams.set('tool', tool);
   if (operation) url.searchParams.set('operation', operation);
   link.href = url.href;
  }
  for (const event of ['focus','pointerdown','click','contextmenu']) link.addEventListener(event, updateLink);
  for (const control of ['mode','operation','manualToolShape','reliefTool']) {
   document.getElementById(control)?.addEventListener('change', updateLink);
  }
  window.addEventListener('cnc-project-loaded', updateLink);
  updateLink();
  panel.prepend(link);
 }
 addMaterialsGuide(r.cam, 'openMaterialsGuide', 'cam');
 addMaterialsGuide(r.relief, 'reliefMaterialsGuide', 'relief');
 const guideParagraph = document.createElement('p');
 const guideLink = document.createElement('a');
 guideLink.href = './materials-guide.html';
 guideLink.textContent = 'Interactive materials guide';
 guideLink.target = '_blank';
 guideLink.rel = 'noopener';
 guideParagraph.append(guideLink);
 r.help.append(guideParagraph);
 const heading=document.createElement('h2');heading.textContent='Shapes & layout';l.shapes.prepend(heading);
 const header=document.querySelector('header'),footer=document.querySelector('footer');const measure=()=>document.documentElement.style.setProperty('--studio-chrome',`${header.getBoundingClientRect().height+footer.getBoundingClientRect().height}px`);new ResizeObserver(measure).observe(header);new ResizeObserver(measure).observe(footer);measure();
 window.CNCToolTabs={open(side,key){document.getElementById(`${side==='design'?'design':'machine'}-tab-${key}`)?.click();}};
})();
