/* Portable, browser-local CAD macros. Only explicit Run invokes the design workspace. */
(() => {
  'use strict';

  const format = 'kerfkite-cad-macros';
  const storageKey = 'kerfkite.cad-macros.v1';
  const maxFileBytes = 1024 * 1024;
  const maxEntries = 100;
  const maxParameters = 32;
  const enc = new TextEncoder();
  const get = suffix => document.getElementById('cad-macro-' + suffix);
  const list = get('list'), dialog = get('dialog'), table = get('params').tBodies[0];
  const openButton = document.getElementById('cad-macros-open');
  document.getElementById('cad-script-open')?.after(openButton);
  const starters = [
    {name: 'Drawing frame', kind: 'commands', parameters: {width: 60, height: 30},
      source: 'rect {width} {height}\nfit drawing'},
    {name: 'Fastener layout', kind: 'commands', parameters: {radius: 2, left: 8, right: 52, bottom: 8, top: 28},
      source: 'circle {radius} {left} {bottom}\ncircle {radius} {right} {bottom}\ncircle {radius} {right} {top}\ncircle {radius} {left} {top}\nfit drawing'},
    {name: 'Drilled spacer', kind: 'scad', parameters: {outer: 18, bore: 6, height: 8},
      source: '$fn = 64;\ndifference() {\n  cylinder(d=outer, h=height);\n  translate([0,0,-1]) cylinder(d=bore, h=height+2);\n}'},
    {name: 'Rounded tag', kind: 'scad', parameters: {width: 60, height: 24, corner: 4, hole: 5},
      source: '$fn = 48;\ndifference() {\n  hull()\n    for (x=[corner,width-corner], y=[corner,height-corner])\n      translate([x,y]) circle(r=corner);\n  translate([2*corner,height/2]) circle(d=hole);\n}'},
    {name: 'Mounting plate', kind: 'scad', parameters: {width: 70, length: 40, thickness: 5, margin: 7, bore: 4},
      source: '$fn = 48;\ndifference() {\n  cube([width,length,thickness]);\n  for (x=[margin,width-margin], y=[margin,length-margin])\n    translate([x,y,-1]) cylinder(h=thickness+2, d=bore);\n}'},
    {name: 'Flanged bushing', kind: 'scad', parameters: {body: 14, flange: 22, bore: 8, length: 16, lip: 3},
      source: '$fn = 64;\ndifference() {\n  union() {\n    cylinder(d=body, h=length);\n    cylinder(d=flange, h=lip);\n  }\n  translate([0,0,-1]) cylinder(d=bore, h=length+2);\n}'},
    {name: 'Slotted panel', kind: 'scad', parameters: {width: 80, height: 40, slots: 4, slot_length: 34, slot_width: 3},
      source: '$fn = 32;\ndifference() {\n  square([width,height]);\n  for (i=[1:slots])\n    hull()\n      for (x=[(width-slot_length+slot_width)/2,(width+slot_length-slot_width)/2])\n        translate([x,i*height/(slots+1)]) circle(d=slot_width);\n}'}
  ];
  const key = name => name.toLowerCase();
  const isRecord = value => value !== null && typeof value === 'object' && !Array.isArray(value);
  const reserved = new Set(['true', 'false', 'PI', 'if', 'else', 'module', 'function', 'for', 'children',
    '__proto__', ...Object.getOwnPropertyNames(Object.prototype)]);
  const numeric = n => typeof n === 'number' && Number.isFinite(n) && Math.abs(n) <= 100000;
  function validateParameters(parameters) {
    if (!isRecord(parameters) || Object.keys(parameters).length > maxParameters)
      throw Error('Use up to ' + maxParameters + ' named numeric parameters.');
    for (const [name, value] of Object.entries(parameters)) {
      if (!/^[A-Za-z_][A-Za-z0-9_]{0,39}$/.test(name) || reserved.has(name))
        throw Error('Invalid parameter name “' + name + '”. Use 1–40 letters, digits or underscores, starting with a letter or underscore.');
      if (!numeric(value)) throw Error('Enter a finite value from −100000 to 100000 for ' + name + '.');
    }
    return Object.fromEntries(Object.entries(parameters));
  }
  function validateMacro(input) {
    if (!isRecord(input) || typeof input.name !== 'string') throw Error('Each macro needs a name.');
    const name = input.name.trim();
    if (!name || name.length > 64 || /[\u0000-\u001f\u007f=]/.test(name))
      throw Error('Use a macro name of 1–64 characters without controls or an equals sign.');
    if (!['commands', 'scad'].includes(input.kind)) throw Error('Select a command sequence or OpenSCAD-style model.');
    if (typeof input.source !== 'string' || !input.source.trim() || enc.encode(input.source).length > 65536)
      throw Error('Enter macro source of up to 64 KiB.');
    return {name, kind: input.kind, source: input.source, parameters: validateParameters(input.parameters)};
  }
  function validateLibrary(file) {
    if (!isRecord(file) || file.format !== format || file.version !== 1 || !Array.isArray(file.macros))
      throw Error('Choose a KerfKite CAD macro library (JSON, version 1).');
    if (file.macros.length > maxEntries) throw Error('The library supports up to ' + maxEntries + ' macros.');
    const entries = file.macros.map(validateMacro);
    if (new Set(entries.map(entry => key(entry.name))).size !== entries.length)
      throw Error('Every macro in the file needs a unique name.');
    return entries;
  }
  function isImportedCopy(entry, candidate) {
    const suffix = / \(([1-9]\d*)\)$/.exec(candidate.name);
    if (!suffix || Number(suffix[1]) < 2) return false;
    return key(candidate.name) === key(entry.name.slice(0, 64 - suffix[0].length) + suffix[0])
      && same(candidate, entry);
  }
  function parseLibrary(raw) {
    if (enc.encode(raw).length > maxFileBytes) throw Error('Macro libraries must fit within 1 MiB.');
    let parsed;
    try { parsed = JSON.parse(raw); } catch { throw Error('The macro library contains invalid JSON.'); }
    return validateLibrary(parsed);
  }
  const packageEntries = macros => ({format, version: 1, macros});
  function readLibrary() {
    const raw = localStorage.getItem(storageKey);
    return raw === null ? starters.map(validateMacro) : parseLibrary(raw);
  }
  function saveLibrary(entries) {
    const encoded = JSON.stringify(packageEntries(validateLibrary(packageEntries(entries))));
    if (enc.encode(encoded).length > maxFileBytes) throw Error('The combined library exceeds 1 MiB.');
    try { localStorage.setItem(storageKey, encoded); }
    catch { throw Error('The browser could not save the library. Check storage access and available space.'); }
    library = entries;
  }
  const find = (entries, name) => entries.find(entry => key(entry.name) === key(name));
  function uniqueName(base, entries) {
    if (!find(entries, base)) return base;
    for (let i = 2; ; i++) {
      const suffix = ' (' + i + ')', name = base.slice(0, 64 - suffix.length) + suffix;
      if (!find(entries, name)) return name;
    }
  }
  function same(a, b) {
    return a && b && a.kind === b.kind && a.source === b.source
      && Object.keys(a.parameters).length === Object.keys(b.parameters).length
      && Object.entries(a.parameters).every(([name, value]) => b.parameters[name] === value);
  }

  let library = [], original = null, baseline = '', busy = false;
  function status(text = '', error = false) {
    get('status').textContent = text;
    get('status').dataset.error = String(error);
  }
  function errorText(e) { return e?.message || String(e); }
  function renderList() {
    const query = get('search').value.trim().toLowerCase();
    const matches = library.filter(m => key(m.name).includes(query)).sort((a, b) => a.name.localeCompare(b.name));
    list.replaceChildren(...matches.map(m => new Option(m.name, m.name)));
    list.selectedIndex = matches.findIndex(m => m.name === original?.name);
    get('count').textContent = matches.length + (matches.length === 1 ? ' macro' : ' macros')
      + (query ? ' matching your search' : ' in this browser');
  }
  function updateHint() {
    get('source-hint').textContent = get('kind').value === 'commands'
      ? 'One drawing or view command per line. Use {width} or $width for a parameter.'
      : 'Use parameter names directly in the model, for example cube([width,20,5]);';
  }
  function paramRow(name = '', value = '') {
    const row = table.insertRow();
    const inputs = ['name', 'default', 'run'].map((part, index) => {
      const field = document.createElement('input');
      field.className = 'macro-param-' + part;
      field.type = index ? 'number' : 'text';
      if (index) field.step = 'any';
      else { field.maxLength = 40; field.autocomplete = 'off'; }
      field.setAttribute('aria-label', ['Parameter name', 'Saved default', 'Value for this run'][index]);
      row.insertCell().append(field);
      return field;
    });
    inputs[0].value = name;
    inputs[1].value = value;
    inputs[2].placeholder = value;
    inputs[1].addEventListener('input', () => { inputs[2].placeholder = inputs[1].value; });
    const remove = document.createElement('button');
    remove.className = 'macro-param-remove';
    remove.type = 'button'; remove.textContent = '×'; remove.setAttribute('aria-label', 'Remove parameter');
    remove.onclick = () => { row.remove(); refreshDraft(); get('param-add').focus(); };
    row.insertCell().append(remove);
    return inputs[0];
  }
  function draftFingerprint() {
    return JSON.stringify([get('name').value, get('kind').value, get('source').value,
      Array.from(table.rows, row => [row.querySelector('.macro-param-name').value, row.querySelector('.macro-param-default').value])]);
  }
  const dirty = () => baseline !== draftFingerprint();
  function refreshDraft() {
    get('param-empty').hidden = table.rows.length > 0;
    get('params').hidden = table.rows.length === 0;
    get('dirty').textContent = dirty() ? 'Unsaved changes' : '';
    get('param-add').disabled = busy || table.rows.length >= maxParameters;
    get('delete').disabled = busy || !original;
  }
  function loadEditor(entry, isSaved = true) {
    original = isSaved ? entry : null;
    get('name').value = entry?.name || '';
    get('kind').value = entry?.kind || 'commands';
    get('source').value = entry?.source || '';
    table.replaceChildren();
    for (const [name, value] of Object.entries(entry?.parameters || {})) paramRow(name, value);
    baseline = isSaved || !entry ? draftFingerprint() : '';
    renderList(); updateHint(); refreshDraft();
  }
  const discard = () => !dirty() || window.confirm('Discard unsaved macro edits?');
  function readDraft(withOverrides = false) {
    const parameters = Object.create(null);
    for (const row of table.rows) {
      const name = row.querySelector('.macro-param-name').value.trim();
      if (Object.hasOwn(parameters, name)) throw Error('Parameter “' + name + '” appears more than once.');
      const base = row.querySelector('.macro-param-default'), run = row.querySelector('.macro-param-run');
      if (base.value === '' || !numeric(base.valueAsNumber)) throw Error('Enter a numeric default for ' + (name || 'each parameter') + '.');
      if (withOverrides && run.validity.badInput) throw Error('Enter a numeric run value for ' + name + '.');
      parameters[name] = withOverrides && run.value !== '' ? run.valueAsNumber : base.valueAsNumber;
    }
    return validateMacro({name: get('name').value, kind: get('kind').value, source: get('source').value, parameters});
  }
  function currentSaved(entries) {
    if (!original) return null;
    const saved = find(entries, original.name);
    if (!same(saved, original) || saved.name !== original.name)
      throw Error('This macro changed in another window. Duplicate your edits or reopen the saved entry.');
    return saved;
  }
  function saveDraft() {
    const entry = readDraft(), entries = readLibrary(), prior = currentSaved(entries), conflict = find(entries, entry.name);
    if (conflict && conflict !== prior) throw Error('A macro with that name already exists. Choose another name.');
    if (prior) entries[entries.indexOf(prior)] = entry;
    else entries.push(entry);
    // A save updates defaults. Keep any trial values in the editor.
    const trials = Array.from(table.rows, row => row.querySelector('.macro-param-run').value);
    saveLibrary(entries); get('search').value = ''; loadEditor(entry);
    Array.from(table.rows).forEach((row, i) => { row.querySelector('.macro-param-run').value = trials[i]; });
    status('Saved “' + entry.name + '” in this browser.');
  }
  function duplicateDraft() {
    const entry = readDraft();
    entry.name = uniqueName(entry.name, [...readLibrary(), entry]);
    get('search').value = ''; loadEditor(entry, false); get('name').focus(); get('name').select();
    status('Choose a name and save this copy.');
  }
  function deleteSelected() {
    const entries = readLibrary(), prior = currentSaved(entries);
    if (!prior || !window.confirm('Delete “' + prior.name + '” from this library?' + (dirty() ? ' Unsaved edits will also be discarded.' : ''))) return;
    const index = entries.indexOf(prior);
    entries.splice(index, 1); saveLibrary(entries);
    get('search').value = ''; loadEditor(entries[Math.min(index, entries.length - 1)]);
    status('Deleted “' + prior.name + '”.');
  }
  async function importFile(file) {
    if (!file) return;
    if (file.size > maxFileBytes) throw Error('Choose a JSON macro library up to 1 MiB.');
    const incoming = parseLibrary(await file.text()), entries = readLibrary(), added = [];
    for (const entry of incoming) {
      const existing = find(entries, entry.name);
      if (same(entry, existing) || (existing && entries.some(candidate => isImportedCopy(entry, candidate)))) continue;
      const name = uniqueName(entry.name, entries);
      const candidate = {...entry, name};
      entries.push(candidate); added.push(candidate);
    }
    if (!added.length) { status('The selected file contains no new macros.'); return; }
    if (!discard()) return;
    saveLibrary(entries); get('search').value = ''; loadEditor(added[0]);
    status('Imported ' + added.length + (added.length === 1 ? ' macro.' : ' macros.') + ' Conflicting names were kept as separate entries.');
  }
  function exportLibrary() {
    const entries = readLibrary();
    download('kerfkite-cad-macros.json', JSON.stringify(packageEntries(entries)), 'application/json');
    status('Exported ' + entries.length + ' saved macros.' + (dirty() ? ' Save your draft to include its changes.' : ''));
  }
  function setBusy(value) {
    busy = value;
    for (const input of dialog.querySelectorAll('input,select,button,textarea')) input.disabled = value;
    refreshDraft();
  }
  async function execute(entry) {
    if (busy) throw Error('A macro is already running.');
    if (typeof window.KerfKiteCAD?.runMacro !== 'function') throw Error('The CAD workspace is still loading.');
    setBusy(true);
    try {
      const message = await window.KerfKiteCAD.runMacro(entry);
      return typeof message === 'string' && message ? message : 'Ran “' + entry.name + '”.';
    } finally { setBusy(false); }
  }
  async function run(name, values = {}) {
    if (typeof name !== 'string') throw Error('Choose a saved macro name.');
    const entry = find(readLibrary(), name.trim());
    if (!entry) throw Error('No saved macro named “' + name + '”.');
    const overrides = validateParameters(values);
    for (const param of Object.keys(overrides)) {
      if (!Object.hasOwn(entry.parameters, param)) throw Error('Unknown parameter “' + param + '” for ' + entry.name + '.');
    }
    return execute({...entry, parameters: {...entry.parameters, ...overrides}});
  }
  function open() {
    if (dialog.open) return;
    let warning = '';
    try { library = readLibrary(); }
    catch (e) { library = []; warning = 'Cannot load the saved macro library. ' + errorText(e); }
    const last = original && find(library, original.name);
    const first = find(library, 'Drawing frame') || library[0];
    get('search').value = ''; loadEditor(last || first); status(warning, Boolean(warning));
    dialog.showModal(); (library.length ? list : get('name')).focus();
  }
  function close() { if (!busy && discard()) dialog.close(); }
  function report(action) {
    return async () => { try { await action(); } catch (e) { status(errorText(e), true); } };
  }
  openButton.onclick = open;
  get('close').onclick = close;
  dialog.addEventListener('cancel', event => { event.preventDefault(); close(); });
  dialog.addEventListener('input', refreshDraft);
  get('kind').onchange = () => { updateHint(); refreshDraft(); };
  get('search').oninput = renderList;
  list.onchange = () => {
    const choice = find(library, list.value);
    if (!choice || !discard()) { renderList(); return; }
    loadEditor(choice); status();
  };
  get('new').onclick = () => {
    if (!discard()) return;
    get('search').value = ''; loadEditor(null); status(); get('name').focus();
  };
  get('param-add').onclick = () => { if (table.rows.length < maxParameters) paramRow().focus(); refreshDraft(); };
  get('save').onclick = report(saveDraft);
  get('duplicate').onclick = report(duplicateDraft);
  get('delete').onclick = report(deleteSelected);
  get('export').onclick = report(exportLibrary);
  get('import').onclick = () => get('file').click();
  get('file').onchange = report(async () => {
    const file = get('file').files[0]; get('file').value = '';
    await importFile(file);
  });
  get('run').onclick = report(async () => {
    const entry = readDraft(true); status('Running “' + entry.name + '”…');
    status(await execute(entry));
  });
  window.addEventListener('storage', event => {
    if (event.key !== storageKey || !dialog.open || busy) return;
    try {
      library = readLibrary();
      if (dirty()) { renderList(); status('Library changed in another window. Your draft is still open.'); }
      else { loadEditor((original && find(library, original.name)) || library[0]); status('Library refreshed.'); }
    } catch (e) { status(errorText(e), true); }
  });
  function save(entry, replace = false) {
    if (busy) throw Error('Wait for the current macro to finish.');
    if (typeof replace !== 'boolean') throw Error('Use true or false for replace.');
    const validated = validateMacro(entry), entries = readLibrary(), existing = find(entries, validated.name);
    const editing = dialog.open && dirty();
    if (editing && [original?.name, get('name').value.trim()].some(name => name && key(name) === key(validated.name)))
      throw Error('This macro has unsaved edits in the editor.');
    if (existing && !replace) throw Error('A macro with that name already exists.');
    if (existing) entries[entries.indexOf(existing)] = validated;
    else entries.push(validated);
    saveLibrary(entries);
    if (dialog.open) {
      if (!editing && original && key(original.name) === key(validated.name)) loadEditor(validated);
      else renderList();
    }
    return validateMacro(validated);
  }
  window.KerfKiteMacros = Object.freeze({open, run, list: readLibrary, save});
})();
