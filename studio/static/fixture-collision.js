'use strict';

(() => {
  const panel = document.createElement('details');
  panel.id = 'fixtureCollisionPanel';
  panel.className = 'tab-section';
  panel.innerHTML = `<summary>Fixture clearance</summary>
    <p class="hint">Check the posted XYZ moves against clamp boxes in work coordinates. Measure from the tool tip, with stock top at Z zero.</p>
    <fieldset><legend>Mounted head · mm</legend><div class="pair" data-fields="tool"></div></fieldset>
    <fieldset><legend>Starting tool tip · work coordinates, mm</legend><div class="pair" data-fields="start"></div></fieldset>
    <div data-fixtures></div>
    <div class="buttons"><button id="fixtureAdd">Add clamp</button><button id="fixtureCheck">Check generated preview</button></div>
    <div role="status" aria-live="polite" data-results></div>
    <details><summary>Try the clamp clearance lesson</summary>
    <p>Two contours require a rapid from X 10 to X 50 at Y 20. A 2 mm-wide clamp at X 30 reaches the side of the holder. Start at Y 21, then move the clamp to Y 40 and compare the results.</p>
    <label>Lesson clamp minimum Y · mm<input id="fixtureLessonY" type="number" step="any" value="21"></label>
    <button id="fixtureLesson">Run lesson simulation</button><div data-lesson-results role="status" aria-live="polite"></div></details>`;
  document.getElementById('machine-panel-cam').append(panel);
  const $el = selector => panel.querySelector(selector);
  const toolFields = [['diameter', 'Tool envelope diameter'], ['stickout', 'Tip to holder base'],
    ['holder_diameter', 'Holder diameter'], ['holder_length', 'Holder length']];
  const axes = ['X', 'Y', 'Z'];
  function field(parent, title, value, minimum) {
    const label = document.createElement('label'), input = document.createElement('input');
    label.textContent = title;
    input.type = 'number';
    input.step = 'any';
    if (minimum !== undefined) input.min = minimum;
    input.value = typeof value === 'number' && Number.isFinite(value) ? value : '';
    label.append(input);
    parent.append(label);
    return input;
  }
  const toolInputs = Object.fromEntries(toolFields.map(([key, label]) =>
    [key, field($el('[data-fields="tool"]'), label, null, .001)]));
  const startInputs = axes.map(axis => field($el('[data-fields="start"]'), axis));
  let clamps = [], generation = 0, lessonGeneration = 0;
  const results = $el('[data-results]'), lessonResults = $el('[data-lesson-results]');
  function value(input) {
    return Number.isFinite(input.valueAsNumber) ? input.valueAsNumber : null;
  }
  function setup() {
    return {tool: Object.fromEntries(Object.entries(toolInputs).map(([key, el]) => [key, value(el)])),
      start: startInputs.map(value), fixtures: clamps.map(row =>
        ({name: row.name.value, min: row.min.map(value), max: row.max.map(value)}))};
  }
  function stale() {
    generation++;
    results.textContent = '';
  }
  function persist() {
    stale();
    window.projectExtras ??= {};
    window.projectExtras.fixtureSetup = setup();
    window.dispatchEvent(new Event('cnc-project-changed'));
  }
  function addClamp(saved) {
    const box = document.createElement('fieldset'), legend = document.createElement('legend');
    legend.textContent = 'Clamp box · mm';
    const label = document.createElement('label'), name = document.createElement('input');
    label.textContent = 'Fixture name';
    name.maxLength = 80;
    name.value = typeof saved?.name === 'string' ? saved.name : 'Clamp ' + (clamps.length + 1);
    label.append(name);
    const group = document.createElement('div');
    group.className = 'pair';
    const row = {name, min: [], max: []};
    axes.forEach((axis, i) => {
      row.min.push(field(group, axis + ' minimum', saved?.min?.[i]));
      row.max.push(field(group, axis + ' maximum', saved?.max?.[i]));
    });
    const remove = document.createElement('button');
    remove.textContent = 'Remove clamp';
    remove.onclick = () => { clamps = clamps.filter(item => item !== row); box.remove(); persist(); };
    box.append(legend, label, group, remove);
    $el('[data-fixtures]').append(box);
    clamps.push(row);
  }
  function restore() {
    stale();
    const saved = window.projectExtras?.fixtureSetup || {};
    for (const [key, input] of Object.entries(toolInputs)) input.value = saved.tool?.[key] ?? '';
    startInputs.forEach((input, i) => { input.value = saved.start?.[i] ?? ''; });
    clamps = [];
    $el('[data-fixtures]').replaceChildren();
    if (Array.isArray(saved.fixtures)) saved.fixtures.slice(0, 64).forEach(addClamp);
  }
  function show(output, report) {
    output.replaceChildren();
    const summary = document.createElement('p');
    summary.textContent = report.collision
      ? `${report.contact_count} contacts across ${report.moves_checked} posted moves. Touching counts as contact.`
      : `Clear of ${report.fixtures_checked} modelled fixture(s) over ${report.moves_checked} posted moves.`;
    output.append(summary);
    if (report.events.length) {
      const list = document.createElement('ol');
      for (const event of report.events) {
        const item = document.createElement('li');
        item.textContent = `Line ${event.line} · ${event.motion} · ${event.component} touches ${event.fixture} · tip XYZ (${event.position.join(', ')}) mm`;
        list.append(item);
      }
      output.append(list);
      if (report.contact_count > report.events.length) output.append('First 100 contacts shown.');
    }
  }
  $el('#fixtureAdd').onclick = () => {
    if (clamps.length >= 64) { results.textContent = 'Maximum 64 clamp boxes.'; return; }
    addClamp();
    persist();
  };
  panel.addEventListener('input', event => {
    if (event.target.id !== 'fixtureLessonY') persist();
    else { lessonGeneration++; lessonResults.textContent = ''; }
  });
  new MutationObserver(stale).observe(document.getElementById('gcode'), {childList:true, characterData:true, subtree:true});
  $el('#fixtureCheck').onclick = async () => {
    const id = ++generation;
    results.textContent = '';
    try {
      if (!preview || JSON.stringify(project()) !== previewProject) throw Error('Generate a current toolpath preview first.');
      const report = await api('fixture-collision', {...setup(), lines:preview.lines});
      if (generation === id) show(results, report);
    } catch (error) { if (generation === id) results.textContent = error.message; }
  };
  $el('#fixtureLesson').onclick = async () => {
    const id = ++lessonGeneration;
    try {
      const y = value($el('#fixtureLessonY'));
      if (y === null) throw Error('Enter the lesson clamp Y position.');
      const job = await api('generate', {mode:'router', width:100, height:80, feed:500, plunge:100,
        power:8000, depth:1, step:1, safe:4, paths:[[[5,20],[10,20]],[[50,20],[55,20]]]});
      const report = await api('fixture-collision', {lines:job.lines, start:[0,0,4],
        tool:{diameter:2,stickout:4,holder_diameter:12,holder_length:15},
        fixtures:[{name:'Bridge clamp', min:[30,y,7], max:[32,y+2,10]}]});
      if (id === lessonGeneration) show(lessonResults, report);
    } catch (error) { if (id === lessonGeneration) lessonResults.textContent = error.message; }
  };
  window.addEventListener('cnc-project-loaded', restore);
  restore();
})();
