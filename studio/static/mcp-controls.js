/* Installed by the assistant's isolated workshop driver on packaged Studio pages. */
(() => {
  if (window.KerfKiteAssistantControls) return;
  const refs = new WeakMap();
  let next = 0;
  const selector = 'button,input,textarea,select,summary,a[href],canvas,svg[tabindex],[role="button"],[role="tab"],[contenteditable=true]';
  const controls = () => [...document.querySelectorAll(selector)];
  function key(el) {
    if (el.id) return el.id;
    if (!refs.has(el)) refs.set(el,'control~'+(++next));
    return refs.get(el);
  }
  function label(el) {
    return (el.getAttribute('aria-label') || [...(el.labels || [])].map(l => l.innerText).join(' ') ||
      el.title || el.placeholder || el.innerText || el.id || el.tagName).trim().slice(0,160);
  }
  function unopened(el){
    const parents=[];
    for(let p=el.parentElement;p;p=p.parentElement){
      if(p.tagName==='DETAILS'&&!p.open){
        const summary=p.querySelector(':scope > summary');
        if(summary && !summary.contains(el))parents.unshift(key(summary));
      }
    }
    return parents;
  }
  function describe(el) {
    const visible=Boolean(el.getClientRects().length && getComputedStyle(el).visibility!=='hidden');
    return {ref:key(el),label:label(el),type:el.getAttribute('role')||el.type||el.tagName.toLowerCase(),
      visible,disabled:Boolean(el.disabled),panel:el.closest('[role=tabpanel]')?.id||'',openFirst:unopened(el),
      ...(el.type==='checkbox'||el.type==='radio'?{checked:el.checked}:{}),
      ...(el.tagName==='SELECT'?{value:el.value,options:[...el.options].slice(0,100).map(o=>[o.value,o.label])}:{})};
  }
  window.KerfKiteAssistantControls=Object.freeze({
    search({query,offset,limit,visibleOnly}){
      const words=query.toLowerCase().split(/\s+/).filter(Boolean);
      const matches=controls().map(describe).filter(c=>(!visibleOnly||c.visible)&&
        words.every(w=>(c.ref+' '+c.label+' '+c.panel).toLowerCase().includes(w)));
      return {total:matches.length,offset,controls:matches.slice(offset,offset+limit)};
    },
    element(ref){return controls().find(e=>key(e)===ref)||null;}
  });
})();
