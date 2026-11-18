/* Local native desktop clients exchange document data with the same editor as the browser. */
'use strict';
(() => {
  if (window !== window.top || location.protocol !== 'http:' || location.hostname !== '127.0.0.1') return;
  const tabs = new Map([
    ['Design','design-tab-shapes'], ['3D CAD','design-tab-solid'], ['Templates','design-tab-templates'],
    ['Layers','design-tab-layers'], ['Text','design-tab-text'], ['Images','design-tab-images'],
    ['Properties','design-tab-properties'], ['Projects','design-tab-projects'], ['CAM','machine-tab-cam'],
    ['Objects','design-tab-objects'], ['3D relief','machine-tab-relief'], ['Rotary','machine-tab-rotary'],
    ['Quote','machine-tab-quote'], ['Control','machine-tab-control'], ['Help','machine-tab-help']
  ]);
  let ready = false, attached = false, documentId = 0, baseline, currentFingerprint, offeredSave;
  let sequence = 0, pending = null;
  function touch() { currentFingerprint = undefined; }
  function fingerprint() {
    return currentFingerprint ??= CNCProjects.fingerprint();
  }
  function replaceDocument() {
    documentId++;
    offeredSave = null;
    touch();
    baseline = fingerprint();
    // Undo in the new document must never restore objects from its predecessor.
    snapshots = []; historyIndex = -1; window.recordHistory();
  }
  // Only attach listeners and compute document fingerprints when a native client connects.
  function attach() {
    attached = true;
    const previous = invalidate;
    invalidate = function(...args) { touch(); return previous(...args); };
    for (const event of ['input','change']) document.addEventListener(event,touch,true);
    for (const event of ['cnc-project-changed','cnc-layers-changed']) window.addEventListener(event,touch);
    window.addEventListener('cnc-project-loaded',replaceDocument);
    touch();
    if (baseline === undefined) baseline = fingerprint();
  }
  window.studioReady.then(() => { baseline = CNCProjects.fingerprint(); ready = true; }).catch(() => {});
  function state() {
    const o = objects[selected];
    let selection = null;
    if (o && o.paths.some(p => p.length)) {
      const [x,y,right,top] = bounds(o.paths);
      selection = {name:o.name,x,y,width:right-x,height:top-y};
    }
    return {title:window.projectMetadata.name,dirty:fingerprint() !== baseline,documentId,
      objects:objects.map(({name},index) => ({index,name})),selected,selection,mode:$('mode').value};
  }
  function checkArea(x,y,width,height) {
    if (![x,y,width,height].every(Number.isFinite) || x<0 || y<0 || width<0 || height<0 ||
        x+width>Number($('width').value)+1e-8 || y+height>Number($('height').value)+1e-8)
      throw Error('Enter dimensions inside the current work area, in millimetres.');
  }
  function start(work) {
    const job = {id:++sequence};
    pending = job;
    const body = document.body, wasInert = body.inert;
    body.inert = true;
    Promise.resolve().then(work).then(value => { job.reply = {ok:true,value}; },
      error => { job.reply = {ok:false,error:error.message}; }).finally(() => { body.inert = wasInert; });
    return {ok:true,pending:job.id};
  }
  function dispatch(action,p={}) {
    if (!ready) throw Error('The workshop is still starting.');
    if (!attached) attach();
    if (action === 'result') {
      if (!pending || p.id !== pending.id) throw Error('Unknown workshop operation');
      if (!pending.reply) return {ok:true,pending:p.id};
      const reply = pending.reply; pending = null; return reply;
    }
    if (pending && action !== 'state') throw Error('Wait for the current workshop operation to finish.');
    let value;
    switch (action) {
    case 'state': value = state(); break;
    case 'select':
      if (!Number.isInteger(p.index) || p.index < -1 || p.index >= objects.length) throw Error('Select an existing object');
      selected = p.index; draw(); break;
    case 'shape':
      checkArea(p.x,p.y,p.width,p.height);
      if (p.kind === 'line' ? !(p.width || p.height) : !(p.width && p.height)) throw Error('Enter a non-zero shape size');
      insertPrimitive(p.kind,p.x,p.y,p.width,p.height); break;
    case 'move': {
      const o = selectedObject();
      if (o.surface) throw Error('Use relief placement for this object');
      const [x,y,right,top] = CNCLayout.extent(o);
      checkArea(p.x,p.y,right-x,top-y);
      objects = CNCLayout.apply(objects,[{index:selected,dx:p.x-x,dy:p.y-y}]);
      invalidate(); draw(); break;
    }
    case 'undo':
    case 'redo': {
      // Native menu accelerators arrive here before the web view handles text editing.
      const focus = document.activeElement;
      if (focus?.matches('#cad-command, dialog[open] input, dialog[open] textarea, [contenteditable=true]')) document.execCommand(action);
      else if (!document.querySelector('dialog[open]')) restore(action === 'undo' ? -1 : 1);
      break;
    }
    case 'command-line': window.KerfKiteCAD.focusCommand(); break;
    case 'script-editor': window.KerfKiteCAD.openSelected(); break;
    case 'tab': {
      const tab = document.getElementById(tabs.get(p.name));
      if (!tab) throw Error('Unknown workspace tab');
      tab.click(); break;
    }
    case 'preview': return start(async () => { await generateDesignPreview(); return state(); });
    case 'new': CNCProjects.newProject(); break;
    case 'open':
      if (typeof p.content !== 'string' || p.content.length > 4000000) throw Error('Project exceeds the 4 MB limit');
      return start(async () => {
        await CNCProjects.openConfirmed(new File([p.content],'Untitled project.cncstudio.json',{type:'application/json'}));
        return state();
      });
    case 'save':
      value = {...artifact('project'),documentId};
      offeredSave = {content:value.content,documentId,fingerprint:CNCProjects.fingerprint(JSON.parse(value.content))};
      break;
    case 'saved':
      if (!offeredSave || offeredSave.documentId !== documentId || offeredSave.content !== p.content)
        throw Error('This save no longer belongs to the current document.');
      baseline = offeredSave.fingerprint; offeredSave = null; touch(); break;
    case 'export': value = artifact('gcode'); break;
    default: throw Error('Unknown desktop command');
    }
    return {ok:true,value:value ?? state()};
  }
  window.KerfKiteDesktop = Object.freeze({dispatch(action,payload) {
    try { return JSON.stringify(dispatch(action,payload)); }
    catch (error) { return JSON.stringify({ok:false,error:error.message}); }
  }});
})();
