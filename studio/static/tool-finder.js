/* Navigation only: selecting a result reveals and focuses its existing control. */
(() => {
  'use strict';
  const trigger=document.createElement('button');
  trigger.id='toolFinderOpen';
  trigger.type='button';
  trigger.setAttribute('aria-haspopup','dialog');
  trigger.setAttribute('aria-keyshortcuts','Control+K Meta+K');
  trigger.append(document.createTextNode('Find a tool'));
  const shortcut=document.createElement('kbd');
  shortcut.setAttribute('aria-hidden','true');
  shortcut.textContent=/Mac|iPhone|iPad/.test(navigator.platform)?'⌘ K':'Ctrl K';
  trigger.append(shortcut);
  const header=document.querySelector('header');
  header.insertBefore(trigger,header.querySelector('.connection'));

  const dialog=document.createElement('dialog');
  dialog.id='toolFinder';
  dialog.setAttribute('aria-labelledby','toolFinderTitle');
  dialog.innerHTML='<div class="finder-title"><h2 id="toolFinderTitle">Find a tool</h2><button type="button" id="toolFinderClose" aria-label="Close tool finder">Close</button></div>'+
    '<label for="toolFinderQuery">Tool, setting or tab</label>'+
    '<input id="toolFinderQuery" type="search" autocomplete="off" spellcheck="false" role="combobox" aria-autocomplete="list" aria-expanded="true" aria-controls="toolFinderResults">'+
    '<div id="toolFinderResults" role="listbox" aria-label="Matching tools"></div><p id="toolFinderEmpty" role="status" hidden>No tools match this search.</p>';
  document.body.append(dialog);
  const input=dialog.querySelector('input');
  const list=dialog.querySelector('[role=listbox]');
  const empty=dialog.querySelector('[role=status]');
  let catalog=[],results=[],selected=-1,previous=null,waitForEnterRelease=false;
  const words=str=>str.normalize('NFD').replace(/[\u0300-\u036f]/g,'').toLowerCase()
    .replace(/[^\p{L}\p{N}]+/gu,' ').trim();
  const compact=str=>str.replace(/\s+/g,' ').trim();
  const shortcuts={
    cornerStart:'trim intersect crossing lines join corner',
    svgImport:'vector drawing import svg file',
    'label-add':'numbered serial label series QR barcode',
    openLibrary:'stock templates artwork library',
    'phone-enable':'android wifi smartphone pendant pairing',
    materialGuideCam:'material stock cutting guide',
    'placement-group':'layout group selected shapes',
    gear:'cog spur gear teeth',
    polygon:'sides polygon',
    rect:'box rectangle'
  };

  function controlName(node) {
    const explicit=node.getAttribute('aria-label')||node.title;
    if(explicit)return compact(explicit);
    const label=node.labels?.[0];
    const source=label||node;
    const copy=source.cloneNode(true);
    for(const nested of copy.querySelectorAll('input,select,textarea,button,svg,option'))nested.remove();
    return compact(copy.textContent)||node.querySelector('img')?.alt||'';
  }
  function available(node,panel) {
    if(node.matches(':disabled,[type="hidden"]'))return false;
    for(let at=node;at&&at!==panel;at=at.parentElement) {
      if(at.hidden || at.getAttribute('aria-hidden')==='true' || getComputedStyle(at).display==='none')return false;
    }
    return true;
  }
  function buildCatalog(){
    const entries=[];
    function collect(container,tab) {
      const title=tab?.textContent.trim()||'Machine connection';
      function add(target,name,section,isTab=false) {
        if(!name)return;
        const id=target.id.replace(/([a-z])([A-Z])/g,'$1 $2');
        const context=isTab?'Workspace tab':section&&section!==name?title+' › '+section:title;
        entries.push({target,tab,name,context,isTab,label:words(name),scope:words(context),
          keywords:words(id+' '+(shortcuts[target.id]||''))});
      }
      if(tab)add(tab,title,'',true);
      for(const target of container.querySelectorAll('summary,button[id],input[id],select[id],textarea[id],a[href]')){
        if(target.closest('dialog')||!available(target,container))continue;
        const section=target.closest('details')?.querySelector(':scope > summary')?.textContent.trim();
        add(target,controlName(target),section);
      }
    }
    for(const panel of document.querySelectorAll('main [role=tabpanel]')){
      const tab=document.getElementById(panel.getAttribute('aria-labelledby'));
      if(tab?.matches('.tool-tablist [role=tab]') && tab.getAttribute('aria-controls')===panel.id) collect(panel,tab);
    }
    collect(header.querySelector('.connection'),null);
    return entries;
  }
  function rank(item,query,parts){
    if(!query)return item.isTab?3:0;
    const all=item.label+' '+item.scope+' '+item.keywords;
    if(!parts.every(term=>all.includes(term)))return -1;
    let score=item.label===query?100:item.label.startsWith(query)?65:item.label.includes(query)?48:0;
    for(const term of parts) {
      if(item.label.split(' ').some(word=>word===term))score+=14;
      else if(item.label.includes(term))score+=8;
      else if(item.keywords.includes(term))score+=5;
    }
    return score;
  }
  function highlight(index,scroll) {
    selected=index;
    for(const [i,option] of [...list.children].entries())option.setAttribute('aria-selected',String(i===index));
    const row=list.children[index];
    if(row){
      input.setAttribute('aria-activedescendant',row.id);
      if(scroll)row.scrollIntoView({block:'nearest'});
    }else input.removeAttribute('aria-activedescendant');
  }
  function search(){
    const query=words(input.value),parts=query.split(' ');
    results=catalog.map(item=>({item,score:rank(item,query,parts)})).filter(hit=>hit.score>=0)
      .sort((a,b)=>b.score-a.score).slice(0,32).map(hit=>hit.item);
    list.replaceChildren();
    for(const [index,item] of results.entries()){
      const option=document.createElement('div');
      option.id='toolFinderResult'+index;
      option.setAttribute('role','option');
      const label=document.createElement('span');label.textContent=item.name;
      const context=document.createElement('small');context.textContent=item.context;
      option.append(label,context);
      option.onmousedown=event=>event.preventDefault();
      option.onclick=()=>reveal(item);
      list.append(option);
    }
    empty.hidden=results.length>0;
    highlight(results.length?0:-1,false);
    list.scrollTop=0;
  }
  function reveal(item){
    if(!item?.target.isConnected)return;
    dialog.close();
    // Only workspace tab buttons have navigation click handlers. Job, geometry,
    // file and controller controls are always reached by focus alone.
    if(item.tab)item.tab.click();
    for(let at=item.target.parentElement;at;at=at.parentElement)if(at.tagName==='DETAILS')at.open=true;
    item.target.focus({preventScroll:true});
    item.target.scrollIntoView({block:'nearest',inline:'nearest'});
  }
  function open(){
    if(document.querySelector('dialog[open]'))return;
    previous=document.activeElement;
    catalog=buildCatalog();
    input.value='';
    search();
    dialog.showModal();
    input.focus();
  }
  function dismiss(){
    dialog.close();
    if(previous?.isConnected)previous.focus({preventScroll:true});
  }
  dialog.querySelector('#toolFinderClose').onclick=dismiss;
  dialog.addEventListener('cancel',event=>{event.preventDefault();dismiss();});
  trigger.onclick=open;
  input.oninput=search;
  document.addEventListener('keydown',event=>{
    if(waitForEnterRelease&&event.key==='Enter'){event.preventDefault();event.stopImmediatePropagation();return;}
    if((event.ctrlKey||event.metaKey)&&!event.altKey&&!event.shiftKey&&event.key.toLowerCase()==='k'){
      if(document.querySelector('dialog[open]')&&!dialog.open)return;
      event.preventDefault();event.stopPropagation();
      if(!event.repeat){if(dialog.open)input.focus();else open();}
      return;
    }
    if(!dialog.open)return;
    if(event.key==='Escape'){event.preventDefault();event.stopPropagation();dismiss();return;}
    if(event.target!==input||event.isComposing)return;
    if(event.key==='ArrowDown'||event.key==='ArrowUp'){
      event.preventDefault();event.stopPropagation();
      if(results.length)highlight((selected+(event.key==='ArrowDown'?1:-1)+results.length)%results.length,true);
    }else if(event.key==='Enter'){
      event.preventDefault();event.stopPropagation();
      if(selected>=0){waitForEnterRelease=true;reveal(results[selected]);}
    }
  },true);
  document.addEventListener('keyup',event=>{
    if(waitForEnterRelease&&event.key==='Enter'){
      waitForEnterRelease=false;event.preventDefault();event.stopImmediatePropagation();
    }
  },true);
  window.addEventListener('blur',()=>{waitForEnterRelease=false;});
})();
