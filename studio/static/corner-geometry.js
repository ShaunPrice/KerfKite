/* Geometry in drawing coordinates (mm). A join is one vertex on two finite segments. */
((root) => {
  'use strict';
  const EPS = 1e-8;
  const distance = (a, b) => Math.hypot(a[0] - b[0], a[1] - b[1]);
  const clamp = t => Math.max(0, Math.min(1, t));
  const lerp = (a, b, t) => a + (b - a) * t;
  const pointAt = (a, b, t) => [lerp(a[0], b[0], t), lerp(a[1], b[1], t)];
  const finite = p => Array.isArray(p) && p.length === 2 && p.every(Number.isFinite);

  function intersection(a, b, c, d) {
    if (![a,b,c,d].every(finite)) return null;
    const rx = b[0]-a[0], ry = b[1]-a[1], sx = d[0]-c[0], sy = d[1]-c[1];
    const r = Math.hypot(rx, ry), s = Math.hypot(sx, sy);
    const det = rx*sy - ry*sx;
    if (r < EPS || s < EPS || Math.abs(det) <= r*s*1e-12) return null;
    const x = c[0]-a[0], y = c[1]-a[1];
    const t = (x*sy - y*sx)/det, u = (x*ry - y*rx)/det;
    if (t < -EPS/r || t > 1+EPS/r || u < -EPS/s || u > 1+EPS/s) return null;
    return {point:pointAt(a, b, clamp(t)), t:clamp(t), u:clamp(u)};
  }

  function nearest(path, target, start = 0, end = path.length - 1) {
    let best = null;
    for (let i = start; i < end; ++i) {
      const a = path[i], b = path[i+1], dx = b[0]-a[0], dy = b[1]-a[1];
      const length2 = dx*dx + dy*dy;
      if (length2 < EPS*EPS) continue;
      const t = clamp(((target[0]-a[0])*dx + (target[1]-a[1])*dy)/length2);
      const point = pointAt(a, b, t), gap = distance(target, point);
      if (!best || gap < best.gap) best = {edge:i, t, gap, point};
    }
    return best;
  }

  // An imported straight line can contain many sampled vertices. Grow each picked
  // edge through collinear forward neighbours, stopping at the first bend or reversal.
  function straightRun(path, edge) {
    let first = edge, last = edge + 1;
    function continues(a, b, c) {
      const dx = c[0]-a[0], dy = c[1]-a[1], length = Math.hypot(dx,dy);
      if (length < EPS) return false;
      const along = ((b[0]-a[0])*dx + (b[1]-a[1])*dy)/length;
      const deviation = Math.abs((b[0]-a[0])*dy - (b[1]-a[1])*dx)/length;
      return along >= 0 && along <= length && deviation <= 0.00005;
    }
    while (first > 0 && continues(path[first-1], path[first], path[last])) --first;
    while (last+1 < path.length && continues(path[first], path[last], path[last+1])) ++last;
    return [first,last];
  }

  function cutSide(path, picked, edge, t, point) {
    const at = edge + t;
    // A pick on the crossing gives no side information unless the crossing is an endpoint.
    let prefix = picked.edge + picked.t < at;
    if (distance(point, picked.point) <= EPS) {
      if (distance(point, path[0]) <= EPS && at < 1) prefix = false;
      else if (distance(point, path.at(-1)) <= EPS && at > path.length-2) prefix = true;
      else throw Error('Pick farther along the side you want to keep.');
    }
    const indices = [];
    if (prefix) {
      for (let i = 0; i <= edge; ++i) indices.push(i);
    } else {
      for (let i = path.length-1; i > edge; --i) indices.push(i);
    }
    while (indices.length && distance(path[indices.at(-1)], point) <= EPS) indices.pop();
    if (!indices.length) throw Error('The chosen side has no length. Pick the longer side.');
    return {indices, edge, t};
  }

  function joinPaths(a, pickA, b, pickB) {
    const targets = [pickA, pickB];
    const paths = [a,b];
    const runs = paths.map((path, i) => {
      if (path.length < 2 || distance(path[0], path.at(-1)) <= EPS)
        throw Error('Choose open lines or open polylines. A closed contour has no loose end to trim.');
      return straightRun(path, targets[i].edge);
    });
    const coarse = intersection(a[runs[0][0]], a[runs[0][1]], b[runs[1][0]], b[runs[1][1]]);
    if (!coarse) throw Error('The chosen segments do not have a single crossing. Pick two intersecting segments.');
    const nearby = paths.map((path, i) => nearest(path, coarse.point, ...runs[i]).edge);
    // Refine on the original edges so imported samples and their depth/power values
    // use the actual local crossing, including a crossing exactly on a sampled vertex.
    for (let i = Math.max(runs[0][0],nearby[0]-1); i <= Math.min(runs[0][1]-1,nearby[0]+1); ++i) {
      for (let j = Math.max(runs[1][0],nearby[1]-1); j <= Math.min(runs[1][1]-1,nearby[1]+1); ++j) {
        const hit = intersection(a[i], a[i+1], b[j], b[j+1]);
        if (!hit) continue;
        const first = cutSide(a, pickA, i, hit.t, hit.point);
        const second = cutSide(b, pickB, j, hit.u, hit.point);
        const path = [...first.indices.map(k=>a[k]), hit.point, ...second.indices.slice().reverse().map(k=>b[k])];
        return {first,second,point:hit.point,path};
      }
    }
    throw Error('The selected edges do not cross. Pick closer to the intended junction.');
  }

  function channel(a, b, join) {
    const first = join.first, second = join.second;
    const x = lerp(a[first.edge],a[first.edge+1],first.t), y = lerp(b[second.edge],b[second.edge+1],second.t);
    if (!Number.isFinite(x) || !Number.isFinite(y) || Math.abs(x-y) > EPS)
      throw Error('Depth or power differs at the crossing. A continuous path needs the same value at the join.');
    return [...first.indices.map(i=>a[i]), x, ...second.indices.slice().reverse().map(i=>b[i])];
  }

  const api = {intersection, nearest, joinPaths, channel};
  root.KerfCornerGeometry = api;
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
})(globalThis);
