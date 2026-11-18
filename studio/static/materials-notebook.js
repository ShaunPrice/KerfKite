'use strict';

(() => {
  const FORMAT = 'kerfkite-material-samples/1';
  const RESULTS = new Map([['pass', 'Passed inspection'], ['adjust', 'Needs adjustment'], ['reject', 'Rejected']]);
  const LIMIT = 1000;
  const memory = new Map();
  let storageMessage = '';
  let current;

  function temporary(message) {
    storageMessage = message + ' Export a backup before closing this tab.';
    return null;
  }

  async function openNotebook() {
    let scope = 'local';
    if (location.protocol !== 'file:') {
      try {
        const response = await fetch('/api/session', {cache: 'no-store'});
        if (response.ok && response.headers.get('content-type')?.includes('application/json')) {
          const session = await response.json();
          if (session.web_mode) {
            if (!session.username) return temporary('Your Studio account could not be identified.');
            scope = 'user/' + session.username;
          }
        } else if (response.status !== 404 || response.redirected) {
          return temporary('The Studio session could not be verified. Notes are temporary.');
        }
      } catch {
        return temporary('The Studio session is unavailable. Notes are temporary.');
      }
    }
    try {
      return await new Promise((resolve, reject) => {
        const request = indexedDB.open('KerfKite material samples/' + scope, 1);
        request.onupgradeneeded = () => request.result.createObjectStore('samples', {keyPath: 'id'});
        request.onerror = () => reject(request.error);
        request.onsuccess = () => resolve(request.result);
      });
    } catch {
      return temporary('This browser cannot store notes permanently.');
    }
  }
  const database = openNotebook();

  // Read and update in one transaction so concurrent tabs cannot discard each other's samples.
  async function withSamples(mode, action) {
    const db = await database;
    if (!db) return action([...memory.values()], {put: row => memory.set(row.id, row), delete: id => memory.delete(id)});
    return new Promise((resolve, reject) => {
      const transaction = db.transaction('samples', mode);
      const store = transaction.objectStore('samples');
      const read = store.getAll();
      let value;
      let failure;
      read.onsuccess = () => {
        try { value = action(read.result, store); }
        catch (error) { failure = error; transaction.abort(); }
      };
      transaction.oncomplete = () => resolve(value);
      transaction.onabort = () => reject(failure || transaction.error || new Error('Notebook update interrupted.'));
      transaction.onerror = () => reject(transaction.error || new Error('Could not access the notebook.'));
    });
  }

  function validate(row) {
    const text = (value, max) => typeof value === 'string' && value.length <= max;
    const tool = KerfMaterialsData.tools.find(item => item.id === row?.tool);
    if (!tool?.operations.some(item => item.id === row.operation) ||
        !KerfMaterialsData.materials.some(item => item.id === row.material) ||
        !text(row.id, 64) || !/^[a-z0-9-]{1,64}$/i.test(row.id) ||
        !text(row.date, 32) || !Number.isFinite(Date.parse(row.date)) ||
        !RESULTS.has(row.result) || !text(row.stock, 100) || !text(row.notes, 1000) ||
        !row.notes.trim() || !text(row.settings, 300) ||
        (row.thickness !== null && !(Number.isFinite(row.thickness) && row.thickness > 0))) {
      throw new Error('A sample has missing or invalid values.');
    }
    const {id, date, material, operation, stock, thickness, settings, result, notes} = row;
    return {id, date, material, tool: tool.id, operation, stock, thickness, settings, result, notes};
  }

  const template = `
    <h2>Sample notebook</h2>
    <p id="sample-scope"></p>
    <p id="sample-storage"></p>
    <form id="sample-form">
      <div class="sample-pair">
        <label>Stock, supplier or batch <input id="sample-stock" maxlength="100" placeholder="e.g. birch ply, batch B"></label>
        <label>Measured thickness (mm) <input id="sample-thickness" type="number" step="any" min="0" inputmode="decimal"></label>
      </div>
      <label>Machine, tool and settings used <input id="sample-settings" maxlength="300"></label>
      <label>Inspection result <select id="sample-result">
        <option value="adjust">Needs adjustment</option><option value="pass">Passed inspection</option><option value="reject">Rejected</option>
      </select></label>
      <label>Observations <textarea id="sample-notes" rows="2" maxlength="1000" required></textarea></label>
      <div class="sample-actions"><button type="submit" id="sample-save">Save sample</button><button type="button" id="sample-cancel" hidden>Cancel edit</button></div>
    </form>
    <p id="sample-avoid" hidden>This combination is marked Avoid. Existing observations are available below.</p>
    <div id="sample-entries"></div>
    <div class="sample-actions">
      <button type="button" id="sample-export">Export notebook</button>
      <label class="sample-import-label">Import notebook <input type="file" id="sample-import" accept=".json,application/json"></label>
    </div>
    <p id="sample-status" role="status"></p>`;

  function show(host, {material, tool, operation, status}) {
    const view = {};
    current = view;
    host.innerHTML = template;
    const get = id => host.querySelector('#sample-' + id);
    const form = get('form');
    const matches = row => row.material === material.id && row.tool === tool.id && row.operation === operation.id;
    let editing = null;
    const active = () => current === view && host.contains(form);
    const announce = text => { if (active()) get('status').textContent = text; };
    get('scope').textContent = [material.name, tool.label, operation.label].join(' · ');
    form.hidden = status === 'avoid';
    get('avoid').hidden = !form.hidden;
    get('notes').placeholder = 'Describe the edge, surface or backing after inspection.';

    function reset() {
      editing = null;
      form.reset();
      get('save').textContent = 'Save sample';
      get('cancel').hidden = true;
    }
    get('cancel').onclick = reset;

    async function list() {
      const rows = await withSamples('readonly', all => all.filter(matches).sort((a,b) => Date.parse(b.date) - Date.parse(a.date)));
      if (!active()) return;
      get('storage').textContent = storageMessage || 'Stored in this browser. Export your notebook to take it to another machine.';
      const entries = get('entries');
      entries.replaceChildren();
      if (!rows.length) entries.textContent = 'No samples recorded for this selection.';
      for (const row of rows) {
        const entry = document.createElement('article');
        const title = document.createElement('strong');
        title.textContent = [RESULTS.get(row.result), row.stock, row.thickness !== null ? row.thickness + ' mm' : ''].filter(Boolean).join(' · ');
        const details = document.createElement('p');
        details.textContent = [new Date(row.date).toLocaleDateString(), row.settings].filter(Boolean).join(' · ');
        const notes = document.createElement('p');
        notes.textContent = row.notes;
        entry.append(title, details, notes);
        if (status !== 'avoid') {
          const edit = document.createElement('button');
          edit.textContent = 'Edit sample';
          edit.onclick = () => {
            editing = row;
            for (const field of ['stock','thickness','settings','result','notes']) get(field).value = row[field] ?? '';
            get('save').textContent = 'Save changes';
            get('cancel').hidden = false;
            get('stock').focus();
          };
          entry.append(edit);
        }
        const remove = document.createElement('button');
        remove.textContent = 'Delete sample';
        remove.onclick = async () => {
          if (!confirm('Delete this sample from your notebook?')) return;
          try {
            await withSamples('readwrite', (_, store) => store.delete(row.id));
            if (!active()) return;
            if (editing?.id === row.id) reset();
            await list();
            announce('Sample deleted.');
          } catch (error) { announce(error.message); }
        };
        entry.append(remove);
        entries.append(entry);
      }
    }
    form.onsubmit = async event => {
      event.preventDefault();
      if (status === 'avoid') return;
      const original = editing;
      const row = {
        id: original?.id || Array.from(crypto.getRandomValues(new Uint8Array(16)), byte => byte.toString(16).padStart(2, '0')).join(''),
        date: original?.date || new Date().toISOString(), material: material.id, tool: tool.id, operation: operation.id,
        stock: get('stock').value.trim(), thickness: get('thickness').value === '' ? null : get('thickness').valueAsNumber,
        result: get('result').value, settings: get('settings').value.trim(), notes: get('notes').value.trim()
      };
      try {
        validate(row);
        get('save').disabled = true;
        await withSamples('readwrite', (rows, store) => {
          if (original && !rows.some(item => JSON.stringify(item) === JSON.stringify(original))) throw new Error('This sample changed in another tab. Reopen it to edit.');
          if (!original && rows.length >= LIMIT) throw new Error('Notebook full. Export a backup and remove old samples.');
          store.put(row);
        });
        if (!active()) return;
        reset();
        await list();
        announce(storageMessage || 'Sample saved to this browser.');
      } catch (error) { announce(error.message); }
      finally { if (active()) get('save').disabled = false; }
    };
    get('export').onclick = async () => {
      try {
        const samples = await withSamples('readonly', rows => rows);
        const url = URL.createObjectURL(new Blob([JSON.stringify({format: FORMAT, samples}, null, 2)], {type:'application/json'}));
        const link = document.createElement('a');
        link.href = url;
        link.download = 'kerfkite-material-samples.json';
        link.click();
        setTimeout(() => URL.revokeObjectURL(url), 30000);
        announce('Exported ' + samples.length + ' samples from the whole notebook.');
      } catch (error) { announce(error.message); }
    };
    get('import').onchange = async event => {
      const file = event.target.files[0];
      event.target.value = '';
      if (!file) return;
      try {
        if (file.size > 16 * 1024 * 1024) throw new Error('Notebook file exceeds 16 MB.');
        const input = JSON.parse(await file.text());
        if (input?.format !== FORMAT || !Array.isArray(input.samples) || input.samples.length > LIMIT) throw new Error('Choose a KerfKite materials notebook with up to 1,000 samples.');
        const samples = input.samples.map(validate);
        const added = await withSamples('readwrite', (rows, store) => {
          const existing = new Set(rows.map(row => row.id));
          const fresh = new Map(samples.filter(row => !existing.has(row.id)).map(row => [row.id, row]));
          if (rows.length + fresh.size > LIMIT) throw new Error('Import would exceed the notebook limit of 1,000 samples.');
          for (const row of fresh.values()) store.put(row);
          return fresh.size;
        });
        if (!active()) return;
        await list();
        announce('Imported ' + added + ' samples. Kept existing entries.');
      } catch (error) { announce('Import failed. ' + error.message); }
    };
    list().catch(error => announce(error.message));
  }
  window.KerfMaterialsNotebook = {show};
})();
