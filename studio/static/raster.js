(() => {
  'use strict';
  const workshop=window.CNCLaserWorkshop;
  const group=$('rasterFile').closest('details');
  const settings=document.createElement('section');
  const numeric=(id,title,value,min,max,step='any')=>`<label>${title}<input id="${id}" type="number" value="${value}" min="${min}" max="${max}" step="${step}"></label>`;
  settings.innerHTML=`
    <h3>Image adjustments</h3><div class="pair">
    ${numeric('rasterBrightness','Brightness · %',0,-100,100)}${numeric('rasterContrast','Contrast · %',0,-100,100)}
    ${numeric('rasterGamma','Gamma',1,.1,5)}${numeric('rasterThreshold','Threshold · %',50,0,100)}</div>
    <details><summary>Crop &amp; mask</summary><p class="hint">Crop from each source edge. The remaining area fills the destination rectangle.</p>
    <div class="pair">${['Left','Top','Right','Bottom'].map(s=>numeric('rasterCrop'+s,s+' crop · %',0,0,99.99)).join('')}</div>
    <label>Mask<select id="rasterMask"><option value="none">Image alpha</option><option value="ellipse">Ellipse</option><option value="selection">Selected artwork · closed contours</option></select></label></details>
    <h3>Placement</h3><div class="pair">${[['X',10],['Y',10],['W',40],['H',30]].map(([k,v])=>numeric('raster'+k,({W:'Width',H:'Height'}[k]||k)+' · mm',v,k==='X'||k==='Y'?0:.001,2000)).join('')}</div>
    <div class="buttons"><button id="rasterSample">Try tonal wedge</button><button id="rasterRestore">Recall selected raster settings</button></div>
    <canvas id="rasterTonePreview" width="240" height="120" style="width:100%;max-height:145px;object-fit:contain;image-rendering:pixelated" aria-label="Predicted image power. Black is maximum power."></canvas>
    <p id="rasterToneStatus" role="status" class="hint">Choose an image or try the tonal wedge.</p>`;
  $('addRaster').before(settings);
  const grid=document.createElement('details');
  grid.innerHTML=`<summary>Laser material test</summary><p class="hint">Power increases to the right; speed increases upwards. Cells use the controller S scale given below.</p>
    <div class="pair">${[
      ['GridX','Grid X · mm',10,0,2000],['GridY','Grid Y · mm',10,0,2000],
      ['FeedMin','Lowest feed · mm/min',600,1,10000],['FeedMax','Highest feed · mm/min',1200,1,10000],
      ['PowerMin','Lowest power · %',10,.01,100],['PowerMax','Highest power · %',30,.01,100],
      ['GridRows','Feed steps',3,2,10],['GridCols','Power steps',3,2,10],
      ['CellSize','Cell size · mm',6,1,100],['Gap','Gap · mm',2,.1,100],
      ['MaxS','Full-scale S',1000,1,100000],['Hatch','Hatch interval · mm',.5,.05,10]
    ].map(([k,t,v,l,h])=>numeric('material'+k,t,v,l,h)).join('')}</div>
    <label>Cell pattern<select id="materialPattern"><option value="outline">Square outline</option><option value="hatch">Hatched patch</option></select></label>
    <button id="materialGridAdd">Insert test grid</button>`;
  group.append(grid);
  const read=id=>$(id).valueAsNumber;
  let source=null, name='', revision=0, pending;
  function config() {
    return {x:read('rasterX'),y:read('rasterY'),width:read('rasterW'),height:read('rasterH'),interval:read('rasterInterval'),
      overscan:read('rasterOverscan'),brightness:read('rasterBrightness'),contrast:read('rasterContrast'),gamma:read('rasterGamma'),
      threshold:read('rasterThreshold'),style:$('rasterStyle').value,mask:$('rasterMask').value,invert:$('rasterInvert').checked,
      crop:Object.fromEntries(['Left','Top','Right','Bottom'].map(key=>[key.toLowerCase(),read('rasterCrop'+key)]))};
  }
  function prepare() {
    if(!source)throw Error('Choose an image or try the tonal wedge.');
    const opts=config(),[cols,rows]=workshop.scanSize(opts);
    if(opts.mask==='selection')opts.contours=objects[selected]?.paths;
    if(!Number.isFinite(opts.x)||!Number.isFinite(opts.y))throw Error('Set X and Y coordinates.');
    const rect=workshop.sourceRect(source.width,source.height,opts.crop);
    const canvas=document.createElement('canvas');canvas.width=cols;canvas.height=rows;
    const ctx=canvas.getContext('2d',{willReadFrequently:true});
    ctx.drawImage(source,...rect,0,0,cols,rows);
    return {opts,map:workshop.powerMap(ctx.getImageData(0,0,cols,rows).data,opts)};
  }
  function paint() {
    const out=$('rasterTonePreview'), ctx=out.getContext('2d');
    try{
      const {map}=prepare();out.width=map.cols;out.height=map.rows;
      const preview=ctx.createImageData(map.cols,map.rows);
      map.values.forEach((power,i)=>{const v=Math.round((1-power)*255);preview.data.set([v,v,v,255],4*i);});
      ctx.putImageData(preview,0,0);$('rasterToneStatus').textContent=name+' · '+map.cols+' × '+map.rows+' samples. Black is maximum power.';
    }catch(err){ctx.clearRect(0,0,out.width,out.height);$('rasterToneStatus').textContent=err.message;}
  }
  group.addEventListener('input',e=>{if(e.target.id.startsWith('raster')&&e.target!==$('rasterFile')){clearTimeout(pending);pending=setTimeout(paint,60);}});
  function replaceSource(image,label){source?.close?.();source=image;name=label;paint();}
  $('rasterFile').onchange=task(async()=>{
    const file=$('rasterFile').files[0],stamp=++revision;
    replaceSource(null,'');
    if(!file||file.size>8e6)throw Error('Choose an image up to 8 MB');
    const image=await createImageBitmap(file);
    if(stamp!==revision){image.close();return;}
    replaceSource(image,file.name);
  });
  $('rasterSample').onclick=()=>{
    ++revision;$('rasterFile').value='';
    const sample=document.createElement('canvas');sample.width=160;sample.height=80;
    const c=sample.getContext('2d'), g=c.createLinearGradient(0,0,160,0);g.addColorStop(0,'black');g.addColorStop(1,'white');
    c.fillStyle=g;c.fillRect(0,0,160,80);c.clearRect(68,28,24,24);
    replaceSource(sample,'Tonal wedge');
  };
  $('rasterRestore').onclick=task(()=>{
    const recipe=objects[selected]?.rasterRecipe;
    if(!recipe)throw Error('Select a raster created with the image workshop.');
    for(const [id,key] of Object.entries({X:'x',Y:'y',W:'width',H:'height',Interval:'interval',Overscan:'overscan',Brightness:'brightness',Contrast:'contrast',Gamma:'gamma',Threshold:'threshold',Style:'style',Mask:'mask'}))$('raster'+id).value=recipe[key];
    $('rasterInvert').checked=recipe.invert;
    for(const edge of ['Left','Top','Right','Bottom'])$('rasterCrop'+edge).value=recipe.crop[edge.toLowerCase()];
    paint();message('Processing settings restored. Choose the source image and select any artwork mask to create a new raster.');
  });
  function laserSettings(){
    $('mode').value='laser';$('operation').value='line';modeUI();
  }
  $('addRaster').onclick=task(()=>{
    const {opts,map}=prepare(), geometry=workshop.scanPaths(map,opts);
    if(opts.x-opts.overscan<0||opts.y<0||opts.x+opts.width+opts.overscan>read('width')||opts.y+opts.height>read('height'))throw Error('Raster and overscan must fit within the work area.');
    if(objects.length>=100)throw Error('Studio supports up to 100 ordered operations per job.');
    const recipe={...opts};delete recipe.contours;
    laserSettings();
    const values={mode:'laser',operation:'line',laser_pattern:'continuous',lead_shape:'straight',feed:read('feed'),power:read('power'),passes:read('passes'),lead_in:0,lead_out:0,ramp_in:0,ramp_out:0,start_power:100,end_power:100};
    objects.push({name:'Raster · '+name,...geometry,rasterRecipe:recipe,operationSettings:values});selected=objects.length-1;
    invalidate();draw();message('Raster added with saved laser settings. Preview the scanlines before export.');
  });
  $('materialGridAdd').onclick=task(()=>{
    const settings=Object.fromEntries(Object.entries({x:'GridX',y:'GridY',rows:'GridRows',cols:'GridCols',size:'CellSize',gap:'Gap',feedMin:'FeedMin',feedMax:'FeedMax',powerMin:'PowerMin',powerMax:'PowerMax',maxS:'MaxS',interval:'Hatch'}).map(([k,id])=>[k,read('material'+id)]));
    const cells=workshop.materialGrid({...settings,pattern:$('materialPattern').value,bedWidth:read('width'),bedHeight:read('height')});
    if(objects.length+cells.length>100)throw Error('This grid would exceed the 100-operation job limit.');
    laserSettings();objects.push(...cells);selected=objects.length-1;invalidate();draw();
    message(cells.length+' material test cells added. Each object lists its feed and power. Preview before exporting.');
  });
})();
