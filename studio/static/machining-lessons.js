'use strict';
(() => {
  const byId = id => document.getElementById(id);
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
