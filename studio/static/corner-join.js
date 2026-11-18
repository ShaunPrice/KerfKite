/* Pick each side to retain, preview the exact corner, then commit one history edit. */
(() => {
  'use strict';
  const geometry = window.KerfCornerGeometry;
  const panel = document.createElement('section');
  panel.setAttribute('aria-label','Line editing');
  panel.innerHTML = `<div class="buttons">
    <button type="button" id="cornerDraw">Draw two-point line</button>
    <button type="button" id="cornerStart">Trim &amp; join</button></div>
    <div id="cornerControls" hidden>
      <p id="cornerPrompt" role="status" class="hint"></p>
      <details><summary>Pick by coordinate</summary>
        <div class="pair"><label>X · mm<input id="cornerX" type="number" step="any"></label>
        <label>Y · mm<input id="cornerY" type="number" step="any"></label></div>
        <button type="button" id="cornerPick">Pick point</button></details>
      <div class="buttons"><button type="button" id="cornerApply" class="primary" disabled>Apply join</button>
        <button type="button" id="cornerBack" disabled>Pick again</button>
        <button type="button" id="cornerCancel">Cancel</button></div></div>`;
  $('design-panel-shapes').querySelector('.quick-shapes').after(panel);
  const canvas = $('canvas');
  const overlay = svgNode('g', {id:'cornerOverlay', 'pointer-events':'none', 'aria-hidden':'true'});
  canvas.append(overlay);
  let mode = '', picks = [], proposal = null;
  const hint = text => { $('cornerPrompt').textContent = text; };
  const mmPerPixel = () => {
    const m = canvas.getScreenCTM();
    return m ? 1 / Math.max(0.001,Math.hypot(m.a,m.b)) : 1;
  };
  function markPoint(point) {
    overlay.append(svgNode('circle', {
      cx:point[0], cy:Number($('height').value)-point[1], r:4*mmPerPixel(),
      fill:'white', stroke:'#204b40', 'stroke-width':2, 'vector-effect':'non-scaling-stroke'
    }));
  }
  function trace(path, color, width) {
    overlay.append(svgNode('path',{
      d:'M'+path.map(([x,y]) => x+','+(Number($('height').value)-y)).join(' L'),
      fill:'none', stroke:color, 'stroke-width':width, 'stroke-linecap':'round',
      'stroke-linejoin':'round', 'vector-effect':'non-scaling-stroke'
    }));
  }
  function stop() {
    mode = ''; picks = []; proposal = null;
    overlay.replaceChildren();
    $('cornerControls').hidden = true;
    $('cornerDraw').setAttribute('aria-pressed','false');
    $('cornerStart').setAttribute('aria-pressed','false');
    canvas.style.removeProperty('cursor');
  }
  function redraw() {
    overlay.replaceChildren();
    $('cornerControls').hidden = false;
    $('cornerApply').hidden = mode === 'line';
    $('cornerApply').disabled = !proposal;
    $('cornerBack').disabled = picks.length === 0;
    if (mode === 'line') {
      if (picks[0]) markPoint(picks[0]);
      hint(picks.length ? 'Pick the end of the new line.' : 'Pick the start of the new line.');
    } else if (proposal) {
      trace(proposal.join.path, '#137353', 4);
      markPoint(proposal.join.point);
      const [x,y] = proposal.join.point;
      hint('The green path is retained. Join at X '+Number(x.toFixed(4))+' mm, Y '+Number(y.toFixed(4))+' mm. Apply to replace the selected paths.');
    } else if (picks.length) {
      const p = picks[0];
      trace(p.object.paths[p.pathIndex], '#a86016', 3);
      markPoint(p.point);
      hint('Pick the part of the second crossing line to keep. Click away from the crossing.');
    } else {
      hint('Pick the part of the first line to keep. You can pick two separate open paths inside one object.');
    }
  }
  function start(next) {
    stop();
    mode = next;
    $(mode === 'line' ? 'cornerDraw' : 'cornerStart').setAttribute('aria-pressed','true');
    canvas.style.cursor = 'crosshair';
    redraw();
  }

  function nearestPath(point) {
    const layers = window.CNCDesignLayers.layers();
    let chosen = null;
    const tolerance = 11 * mmPerPixel();
    // Iterate front to back. Keep the frontmost item at equal distance.
    for (let i = objects.length-1; i >= 0; --i) {
      const object = objects[i];
      if (!window.CNCDesignLayers.layerFor(object,layers).visible) continue;
      for (let j = 0; j < object.paths.length; ++j) {
        if (picks[0]?.object === object && picks[0].pathIndex === j) continue;
        const hit = geometry.nearest(object.paths[j],point);
        if (hit && hit.gap <= tolerance && (!chosen || hit.gap < chosen.gap - 1e-8))
          chosen = {...hit, object, pathIndex:j};
      }
    }
    if (!chosen) throw Error('Pick near a visible segment on a separate open path.');
    return chosen;
  }
  const editableFields = new Set(['paths','name','designLayerId','output','operationSettings','rotationPivot','depths','powers']);
  function checkPath(pick) {
    if (pick.object.solidLink)
      throw Error('This geometry follows a CAD source. Detach its CAD link in Properties before trimming.');
    if (Object.keys(pick.object).some(key => !editableFields.has(key)))
      throw Error('This object carries editable source or machining data. Use independent vector geometry for a corner join.');
    const path = pick.object.paths[pick.pathIndex];
    if (Math.hypot(path[0][0]-path.at(-1)[0],path[0][1]-path.at(-1)[1]) < 1e-8)
      throw Error('Choose an open path. Closed contours do not have loose ends.');
  }
  function canonical(value) {
    if (!value || typeof value !== 'object') return value;
    if (Array.isArray(value)) return value.map(canonical);
    return Object.fromEntries(Object.keys(value).sort().map(k=>[k,canonical(value[k])]));
  }
  function plan(a,b) {
    const one = a.object, two = b.object;
    if ((one.designLayerId || 'default') !== (two.designLayerId || 'default') ||
        (one.output === false) !== (two.output === false) ||
        JSON.stringify(canonical(one.operationSettings || {})) !== JSON.stringify(canonical(two.operationSettings || {})))
      throw Error('Use two paths on the same layer with matching output and object CAM settings.');
    const join = geometry.joinPaths(one.paths[a.pathIndex],a,two.paths[b.pathIndex],b);
    const channels = {};
    for (const key of ['depths','powers']) {
      if (Boolean(one[key]) !== Boolean(two[key])) throw Error('Both paths must use matching vertex depth and power data.');
      if (one[key]) channels[key] = geometry.channel(one[key][a.pathIndex],two[key][b.pathIndex],join);
    }
    return {join,channels};
  }
  function pick(point) {
    try {
      if (!point.every(Number.isFinite)) throw Error('Enter finite X and Y coordinates.');
      if (mode === 'line') {
        if (point[0]<0 || point[1]<0 || point[0]>Number($('width').value) || point[1]>Number($('height').value))
          throw Error('Choose a point inside the work area.');
        if (picks.length) {
          const from = picks[0];
          if (Math.hypot(point[0]-from[0],point[1]-from[1])<1e-8) throw Error('Choose a different endpoint.');
          stop();
          window.recordHistory?.();
          addGenerated('Line',[[from,point]]);
          message('Two-point line added.');
          return;
        }
        picks.push(point);
      } else {
        if (proposal) return;
        const target = nearestPath(point);
        checkPath(target);
        if (picks.length) proposal = plan(picks[0],target);
        picks.push(target);
      }
      redraw();
    } catch (error) { hint(error.message); }
  }

  function apply() {
    if (!proposal) return;
    try {
      const [a,b] = picks;
      const result = plan(a,b);
      if (!objects.includes(a.object) || !objects.includes(b.object)) throw Error('The artwork changed. Pick the paths again.');
      const updated = [];
      let joined;
      for (const o of objects) {
        if (o !== a.object && o !== b.object) { updated.push(o); continue; }
        const keep = o.paths.map((_,i)=>i).filter(i => o !== b.object || i !== b.pathIndex);
        if (!keep.length) continue;
        const edited = {...o};
        for (const key of ['paths','depths','powers']) {
          if (o[key]) edited[key] = keep.map(i => o === a.object && i === a.pathIndex ?
            (key === 'paths' ? result.join.path : result.channels[key]) : o[key][i]);
        }
        if (o.rotationPivot) edited.rotationPivot = CNCRotation.relative(edited.paths,CNCRotation.pivot(o));
        if (o === a.object) joined = edited;
        updated.push(edited);
      }
      stop();
      invalidate();
      objects = updated;
      selected = objects.indexOf(joined);
      draw();
      window.recordHistory?.();
      message('Corner joined as one continuous path. Undo restores both originals.');
    } catch (error) { hint(error.message); }
  }

  $('cornerDraw').onclick = () => start('line');
  $('cornerStart').onclick = () => start('join');
  $('cornerBack').onclick = () => { picks.pop(); proposal = null; redraw(); };
  $('cornerCancel').onclick = () => { stop(); message('Line edit cancelled.'); };
  $('cornerApply').onclick = apply;
  $('cornerPick').onclick = () => pick([$('cornerX').valueAsNumber,$('cornerY').valueAsNumber]);

  // Capture before normal canvas node/resize/rotate handlers and keep the active tool in Design.
  canvas.parentElement.addEventListener('pointerdown', event => {
    if (!mode || !canvas.contains(event.target) || event.button !== 0) return;
    event.stopImmediatePropagation(); event.preventDefault();
    pick(canvasPoint(event));
  }, true);
  window.addEventListener('keydown', event => { if (event.key === 'Escape' && mode) stop(); });
  window.addEventListener('cnc-tab-changed', () => { if (mode && $('design-panel-shapes').hidden) stop(); });
  // Any document or CAM mutation makes a pending pick stale.
  const previousInvalidate = invalidate;
  invalidate = function(...args) { if (mode) stop(); return previousInvalidate(...args); };
  stop();
})();
