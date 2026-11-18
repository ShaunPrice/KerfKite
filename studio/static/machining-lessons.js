'use strict';
(() => {
  const byId = id => document.getElementById(id);
  const horizontal = [[5, 25], [75, 25]];
  const vertical = [[40, 45], [40, 5]];
  function practiseTrim() {
    const side = byId('trim-horizontal').value;
    const end = byId('trim-vertical').value;
    const feedback = byId('trim-feedback');
    const preview = byId('trim-result');
    if (!side || !end) {
      preview.removeAttribute('d');
      feedback.textContent = 'Choose the two sides to keep.';
      return;
    }
    const geometry = window.KerfCornerGeometry;
    const join = geometry.joinPaths(horizontal, geometry.nearest(horizontal, horizontal[side === 'left' ? 0 : 1]),
                                    vertical, geometry.nearest(vertical, vertical[end === 'upper' ? 1 : 0]));
    preview.setAttribute('d', 'M' + join.path.map(point => point.join(' ')).join(' L'));
    feedback.textContent = (side === 'left' && end === 'upper' ? 'Inside corner complete.' : 'Try left and upper for the example corner.') +
      ' The retained path has one shared vertex at X 40, Y 25 mm.';
  }
  byId('trim-horizontal').onchange = practiseTrim;
  byId('trim-vertical').onchange = practiseTrim;
  // Each loft quadrant blends one straight side with its corresponding circular arc.
  // The result is an elliptical arc; a zero radius at the base gives a straight line.
  function loftOutline(fraction, outletRadius, centreX) {
    const a = (1 - fraction) * 20;
    const b = (1 - fraction) * 12;
    const r = fraction * outletRadius;
    const x = a + r * Math.SQRT1_2;
    const y = b + r * Math.SQRT1_2;
    const sideX = Math.SQRT2 * a + r;
    const sideY = Math.SQRT2 * b + r;
    return [
      'M', centreX - x, -y,
      'A', sideX, r, 0, 0, 1, centreX + x, -y,
      'A', r, sideY, 0, 0, 1, centreX + x, y,
      'A', sideX, r, 0, 0, 1, centreX - x, y,
      'A', r, sideY, 0, 0, 1, centreX - x, -y,
      'Z'
    ].join(' ');
  }

  function sectionLoft() {
    const diameter = byId('loft-diameter'), offset = byId('loft-offset');
    const z = byId('loft-level').valueAsNumber;
    byId('loft-level-value').textContent = z + ' mm';
    const valid = diameter.validity.valid && offset.validity.valid;
    byId('loft-drawing').style.display = valid ? '' : 'none';
    if (!valid) {
      byId('loft-size').textContent = '';
      byId('loft-feedback').textContent = 'Use a diameter from 8 to 36 mm and an offset from −12 to 12 mm, in 0.5 mm steps.';
      return;
    }
    const fraction = z / 20;
    const size = diameter.valueAsNumber, shift = offset.valueAsNumber;
    const centre = fraction * shift;
    const width = (1 - fraction) * 40 + fraction * size;
    const depth = (1 - fraction) * 24 + fraction * size;
    const format = value => Number(value.toFixed(3));
    byId('loft-outlet').setAttribute('r', size / 2);
    byId('loft-outlet').setAttribute('cx', shift);
    byId('loft-outline').setAttribute('d', loftOutline(fraction, size / 2, centre));
    byId('loft-centre').setAttribute('cx', centre);
    byId('loft-size').textContent = format(width) + ' × ' + format(depth) + ' mm · centre X ' + format(centre) + ' mm';
    let feedback = 'Move to Z = 10 mm to inspect the halfway section.';
    if (z === 10) {
      if (width !== 31) feedback = 'At halfway, each 2 mm change in outlet diameter changes both section dimensions by 1 mm.';
      else if (centre !== 2) feedback = 'The section size matches. Set the outlet offset to twice the target centre displacement.';
      else feedback = 'Target reached. Move the plane toward the outlet to see the rectangle turn into a circle.';
    }
    byId('loft-feedback').textContent = feedback;
  }
  for (const id of ['loft-diameter', 'loft-offset', 'loft-level']) byId(id).oninput = sectionLoft;
  sectionLoft();

  const lessons = {
    router: {title: 'Depth per pass', task: 'Cut a 3 mm groove in three depth passes. Set the maximum step-down to 1 mm and compare the Z moves.', label: 'Step-down (mm)', initial: 3, min: .25, max: 3, step: .25},
    laser: {title: 'Power at the start of a cut', task: 'Ramp from zero power to full job power over the first 5 mm. Inspect the S values at the start of the path.', label: 'Entry ramp length (mm)', initial: 0, min: 0, max: 15, step: 1},
    knife: {title: 'Corner compensation', task: 'Model a blade that trails its pivot by 0.5 mm. Compare the swivel moves with the nominal plate outline.', label: 'Blade offset (mm)', initial: .15, min: .05, max: 1, step: .05}
  };
  const outline = [[15,12],[62,12],[62,38],[15,38],[15,12]];
  let result;
  function resetPreview() {
    result = null;
    byId('step').disabled = true;
    for (const id of ['motion','feedback','metrics','code']) byId(id).replaceChildren();
  }
  function changeLesson() {
    resetPreview();
    const l = lessons[byId('lesson').value];
    byId('title').textContent = l.title;
    byId('challenge').textContent = l.task;
    byId('setting-label').textContent = l.label;
    Object.assign(byId('setting'), {min:l.min, max:l.max, step:l.step, value:l.initial});
  }
  function draw() {
    const group = byId('motion');
    group.replaceChildren();
    const line = (d, color, width, dash) => {
      const path = document.createElementNS('http://www.w3.org/2000/svg','path');
      path.setAttribute('d', d); path.setAttribute('stroke', color);
      path.setAttribute('fill', 'none'); path.setAttribute('stroke-width', width);
      if (dash) path.setAttribute('stroke-dasharray', dash);
      group.append(path);
    };
    line('M15 -12H62V-38H15Z','#8eaaad',.3,'1 1');
    const count = Math.round(result.segments.length * byId('step').value / 100);
    for (const [x,y,nx,ny,z,power] of result.segments.slice(0,count)) {
      const color = byId('lesson').value === 'laser' ? 'hsl(24 85% '+(75-40*power/750)+'%)' : '#0c6668';
      line('M'+x+' '+(-y)+'L'+nx+' '+(-ny), color, .45);
    }
    const last = result.segments[count-1];
    byId('metrics').textContent = count+' of '+result.segments.length+' cut segments'+
      (last ? ' · Z '+last[4].toFixed(2)+' mm' : '')+' · '+result.cut_length+' mm total';
  }
  document.querySelector('form').onsubmit = event => {
    event.preventDefault();
    resetPreview();
    const mode = byId('lesson').value, setting = byId('setting').valueAsNumber;
    const project = {mode, protocol:'grbl', paths:[outline], width:80,height:55,operation:'line',
      depth:mode==='knife'?.2:3, safe:5, step:mode==='router'?setting:1, passes:1, feed:400,plunge:100,power:750,
      knife_offset:mode==='knife'?setting:.25, knife_lead:2, ramp_in:mode==='laser'?setting:0, start_power:0};
    try {
      result = CNCOffline.generate(project);
      byId('step').disabled = false; byId('step').value = 100;
      byId('code').textContent = result.lines.join('\n');
      draw();
      const target = {router:1,laser:5,knife:.5}[mode];
      byId('feedback').textContent = setting===target ? 'Lesson complete. Inspect the moves below or choose another head.' : 'Preview ready. Compare the result with the lesson target.';
    } catch(error) { byId('feedback').textContent = error.message; }
  };
  byId('step').oninput = draw;
  byId('lesson').onchange = changeLesson;
  byId('setting').oninput = resetPreview;
  changeLesson();
})();
