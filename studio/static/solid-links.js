/* Associations between a part and its derived machining contours. Coordinates are in millimetres. */
(() => {
  'use strict';

  function currentRevision() {
    return window.projectExtras?.solidRevision;
  }
  function newRevision() {
    return window.projectExtras.solidRevision = crypto.randomUUID();
  }
  function stale(o) {
    return o.solidLink && (o.solidLink.version !== 1 || !currentRevision() || o.solidLink.revision !== currentRevision());
  }
  function requireCurrent() {
    const layers = CNCDesignLayers.layers();
    const o = objects.find(o => stale(o) && o.output !== false && CNCDesignLayers.layerFor(o, layers).output);
    if (o) throw Error('“' + o.name + '” needs its CAD source rebuilt. Open 3D CAD and rebuild, or select the object and detach its CAD link.');
  }
  function place(rings, mesh, placement) {
    if (!rings.length) throw Error('No contours at the selected plane.');
    const [x,y] = placement;
    const [x0,y0,,x1,y1] = mesh.bounds;
    if (![x,y].every(Number.isFinite) || x < 0 || y < 0 ||
        x + x1 - x0 > Number($('width').value) + 1e-8 || y + y1 - y0 > Number($('height').value) + 1e-8) {
      throw Error('The rebuilt part does not fit at its placement in the work area.');
    }
    return rings.map(path => path.map(([px,py]) => [x + (px-x0), y + (py-y0)]));
  }
  async function geometryKey(paths, depths) {
    const [x,y] = bounds(paths);
    // Ignore translations and sub-nanometre noise from pointer arithmetic.
    const points = paths.map(path => path.map(([px,py]) => [Math.round((px-x)*1e8), Math.round((py-y)*1e8)]));
    const bytes = new TextEncoder().encode(JSON.stringify(depths ? [points,depths] : points));
    const hash = new Uint8Array(await crypto.subtle.digest('SHA-256', bytes));
    return Array.from(hash, byte => byte.toString(16).padStart(2,'0')).join('');
  }
  async function linkFor(paths, placement, sectionZ) {
    const revision = currentRevision() || newRevision();
    return {version:1, revision, sectionZ, placement, min:bounds(paths).slice(0,2), key:await geometryKey(paths)};
  }

  // File identity and source revision keep an unrelated STL (including one with the
  // same name) independent and prevent a stale prepared file becoming a new CAD link.
  const preparedReliefs = new WeakMap();
  let documentEpoch = 0;
  window.addEventListener('cnc-project-loaded', () => { ++documentEpoch; });
  function sizes(mesh) {
    return [0,1,2].map(axis => mesh.bounds[axis+3] - mesh.bounds[axis]);
  }
  function stageRelief(file, mesh) {
    preparedReliefs.set(file, {revision:currentRevision() || newRevision(), size:sizes(mesh), epoch:documentEpoch});
  }
  function reliefStage(file) {
    const source = preparedReliefs.get(file);
    if (source && (source.revision !== currentRevision() || source.epoch !== documentEpoch)) {
      throw Error('The staged relief belongs to an earlier CAD revision. Prepare the current part in CAM again.');
    }
    return source;
  }
  async function stlContent(blob) {
    const bytes = new Uint8Array(await blob.arrayBuffer());
    let binary = '';
    for (let i = 0; i < bytes.length; i += 8192) binary += String.fromCharCode(...bytes.subarray(i,i+8192));
    return btoa(binary);
  }
  async function reliefLink(file, data, result) {
    const source = reliefStage(file);
    if (!source) return null;
    const scale = [data.width,data.height,data.depth].map((v,i) => v/source.size[i]);
    if (!scale.every(v => Number.isFinite(v) && v > 0)) throw Error('The relief needs a solid with a positive XYZ size.');
    const {tool_diameter,tool_shape,stepover,allowance,resolution,x,y} = data;
    const key = await geometryKey(result.paths,result.path_depths);
    reliefStage(file);
    return {kind:'relief', version:1, revision:source.revision, placement:[x,y], min:bounds(result.paths).slice(0,2),
      scale, tool:{tool_diameter,tool_shape,stepover,allowance,resolution}, key};
  }
  function reliefSettings(link, mesh, placement) {
    if (!Array.isArray(link.scale) || link.scale.length !== 3 || !link.scale.every(v=>Number.isFinite(v) && v>0) || !link.tool)
      throw Error('Invalid linked relief dimensions or tool settings.');
    const [width,height,depth] = sizes(mesh).map((v,i)=>v*link.scale[i]);
    const [x,y] = placement;
    if (x < -1e-8 || y < -1e-8 || x+width > Number($('width').value)+1e-8 || y+height > Number($('height').value)+1e-8)
      throw Error('The rebuilt relief does not fit at its placement in the work area.');
    // Read only the supported recipe fields. The source mesh always comes from the active part.
    const {tool_diameter,tool_shape,stepover,allowance,resolution} = link.tool;
    return {width,height,depth,x,y,tool_diameter,tool_shape,stepover,allowance,resolution};
  }

  async function refresh(model, mesh, valid, getStl) {
    const revision = currentRevision();
    const candidates = objects.filter(stale);
    if (!candidates.length || !revision) return [];
    const sections = new Map();
    let reliefContent;
    const updates = [];
    const errors = [];
    for (const o of candidates) {
      const link = o.solidLink;
      try {
        const pair = p => Array.isArray(p) && p.length === 2 && p.every(Number.isFinite);
        const relief = link.kind === 'relief';
        if (link.version !== 1 || !pair(link.placement) || !pair(link.min) ||
            (!relief && (link.kind || (link.sectionZ !== null && !Number.isFinite(link.sectionZ)))))
          throw Error('Unsupported machining link.');
        const original = JSON.stringify([o.paths,o.depths]);
        if (o.powers || (relief ? !o.depths : o.depths) || await geometryKey(o.paths, o.depths) !== link.key) {
          throw Error('The drawing geometry was edited. Detach it to keep the edit, or replace it from CAD.');
        }
        if (!valid()) return [];
        const min = bounds(o.paths);
        const placement = link.placement.map((v,i) => v + (min[i] - link.min[i]));
        if (relief) {
          const settings = reliefSettings(link,mesh,placement);
          reliefContent ??= getStl().then(stlContent);
          const result = await api('relief',{...settings,content_base64:await reliefContent});
          if (!valid()) return [];
          const next = {...link,revision,placement,min:bounds(result.paths).slice(0,2),
            key:await geometryKey(result.paths,result.path_depths)};
          updates.push({o,link,original,paths:result.paths,depths:result.path_depths,surface:result.surface,next});
          continue;
        }
        let rings = mesh.profiles;
        if (link.sectionZ !== null) {
          if (!sections.has(link.sectionZ)) sections.set(link.sectionZ, await api('solid-cad/section', {model, z:link.sectionZ}));
          rings = sections.get(link.sectionZ).profiles;
        }
        const paths = place(rings, mesh, placement);
        const next = await linkFor(paths, placement, link.sectionZ);
        updates.push({o, link, original, paths, next});
      } catch (error) {
        errors.push(o.name + ' · ' + error.message);
      }
    }
    if (!valid() || revision !== currentRevision()) return [];
    let changed = false;
    for (const {o, link, original, paths, depths, surface, next} of updates) {
      if (!objects.includes(o) || o.solidLink !== link) continue;
      if (JSON.stringify([o.paths,o.depths]) !== original) {
        errors.push(o.name + ' changed during rebuild. Rebuild again.');
        continue;
      }
      o.paths = paths;
      if (depths) { o.depths = depths; o.surface = surface; }
      o.solidLink = next;
      changed = true;
    }
    if (changed) { invalidate(); draw(); }
    return errors;
  }

  const fields = $('propertyFields');
  const detach = document.createElement('button');
  detach.id = 'solid-detach';
  detach.textContent = 'Detach CAD link and keep current drawing';
  const report = document.createElement('p');
  report.className = 'hint';
  fields.append(report,detach);
  function selection() {
    const o = objects[selected];
    detach.hidden = report.hidden = !o?.solidLink;
    if (o?.solidLink) report.textContent = (o.solidLink.kind === 'relief' ? 'CAD relief' : 'CAD contour') +
      (stale(o) ? ' · awaiting a source rebuild' : ' · linked to the current part');
  }
  detach.onclick = () => {
    const o = objects[selected];
    if (!o?.solidLink) return;
    delete o.solidLink;
    invalidate(); draw(); selection();
    message('The current 2D paths can now be edited independently.');
  };
  const previous = draw;
  draw = function(...args) { const result = previous(...args); selection(); return result; };
  selection();
  window.CNCSolidLinks = {newRevision, stale, requireCurrent, refresh, place, linkFor, stageRelief, reliefStage, reliefLink, stlContent};
})();
