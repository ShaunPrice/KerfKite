/* Label geometry is in millimetres, in design coordinates with Y up. */
((root) => {
  'use strict';
  function sequence(pattern, count, start, increment, digits) {
    if (!pattern.trim() || pattern.length > 120) throw Error('Enter 1–120 characters for the label.');
    for (const [value, min, max] of [[count,1,100],[start,0,99999999],[increment,1,99999999],[digits,1,8]]) {
      if (!Number.isInteger(value) || value < min || value > max) throw Error('Check the number of labels, first number, increment and digits.');
    }
    if (count > 1 && !pattern.includes('{n}')) throw Error('Include {n} in the label to insert the serial number.');
    return Array.from({length:count}, (_, i) => pattern.replaceAll('{n}', String(start+i*increment).padStart(digits,'0')));
  }

  // In each group, two of the five bars are wide. The group selects the wide space.
  function code39(text) {
    const groups = ['1234567890','ABCDEFGHIJ','KLMNOPQRST','UVWXYZ-. *'];
    const wideBars = [0x101,0x041,0x140,0x011,0x110,0x050,0x005,0x104,0x044,0x014];
    const wideSpace = [0x020,0x008,0x002,0x080];
    const punctuation = {'$':0x0a8,'/':0x0a2,'+':0x08a,'%':0x02a};
    if (!/^[0-9A-Z .$+/%-]{1,64}$/.test(text)) throw Error('Code 39 accepts up to 64 uppercase letters, digits, spaces and - . $ / + %.');
    const row = [];
    for (const character of '*'+text+'*') {
      let mask = punctuation[character];
      if (mask === undefined) {
        const group = groups.findIndex(s => s.includes(character));
        mask = wideBars[groups[group].indexOf(character)] | wideSpace[group];
      }
      if (row.length) row.push(false);
      for (let i=0; i<9; i++) {
        const width = mask & (1 << (8-i)) ? 3 : 1;
        for (let j=0;j<width;j++) row.push(i%2===0);
      }
    }
    return {rows:[row],quietX:10,quietY:0};
  }

  function qr(text) {
    if (!text || new TextEncoder().encode(text).length > 160) throw Error('QR data must fit in 160 UTF-8 bytes.');
    const {QrCode,QrSegment} = root.qrcodegen;
    const utf8 = new TextEncoder().encode(text);
    const segments = /^[\x20-\x7e]*$/.test(text) ? QrSegment.makeSegments(text) :
      [QrSegment.makeEci(26),QrSegment.makeBytes(Array.from(utf8))];
    const code = QrCode.encodeSegments(segments,QrCode.Ecc.MEDIUM);
    const rows = Array.from({length:code.size},(_,y)=>Array.from({length:code.size},(_,x)=>code.getModule(x,y)));
    return {rows,quietX:4,quietY:4};
  }

  function engrave(symbol, unit, spacing, height, x, y, budget) {
    if (![unit,spacing,height,x,y].every(Number.isFinite) || unit<0.1 || unit>5 || spacing<0.025 ||
        spacing>unit/2 || height<1 || height>100) throw Error('Use a module of 0.1–5 mm and a line spacing of at most half a module (minimum 0.025 mm).');
    const {rows,quietX,quietY} = symbol;
    const rowHeight=rows.length===1 ? height : unit;
    const passes=Math.ceil(rowHeight/spacing), pitch=rowHeight/passes, paths=[];
    rows.forEach((row,rowIndex)=>{
      const bottom=y+quietY*unit+(rows.length-rowIndex-1)*rowHeight;
      for (let col=0; col<row.length;) {
        if (!row[col]) { col++;continue; }
        const start=col;
        while(col<row.length && row[col])col++;
        // A centreline sits half a hatch pitch inside the dark run on each side.
        const left=x+(quietX+start)*unit+pitch/2, right=x+(quietX+col)*unit-pitch/2;
        if(paths.length+passes>budget)throw Error('Label exceeds the project path limit. Increase line spacing or reduce the series.');
        for(let pass=0;pass<passes;pass++){
          const level=bottom+(pass+.5)*pitch;
          paths.push(pass%2===0?[[left,level],[right,level]]:[[right,level],[left,level]]);
        }
      }
    });
    return {paths,width:(rows[0].length+2*quietX)*unit,height:rows.length*rowHeight+2*quietY*unit};
  }
  const api={sequence,code39,qr,engrave};
  root.KerfLabels=api;
  if(typeof module!=='undefined' && module.exports)module.exports=api;
  if(typeof document==='undefined')return;
  const panel=document.createElement('details');
  panel.id='seriesLabels';
  panel.innerHTML=`<summary>Production labels</summary>
    <label>Label format<select id="label-kind"><option value="text">Engraved text</option><option value="qr">QR code</option><option value="barcode">Code 39 barcode</option></select></label>
    <label>Label content<input id="label-pattern" maxlength="120" value="PART-{n}"></label>
    <p class="hint">Use {n} for a serial number. The first label starts at the Design X and Y position.</p>
    <div class="pair"><label>Number of labels<input id="label-count" type="number" value="1" min="1" max="100"></label><label>Labels per row<input id="label-columns" type="number" value="1" min="1" max="100"></label></div>
    <div class="pair"><label>First number<input id="label-start" type="number" value="1" min="0"></label><label>Increment<input id="label-increment" type="number" value="1" min="1"></label><label>Minimum digits<input id="label-digits" type="number" value="3" min="1" max="8"></label><label>Gap between labels · mm<input id="label-gap" type="number" value="3" min="0" max="200" step=".1"></label></div>
    <div class="pair" id="label-symbol-settings" hidden><label>Module width · mm<input id="label-unit" type="number" value=".8" min=".1" max="5" step=".1"></label><label>Engraving line spacing · mm<input id="label-spacing" type="number" value=".2" min=".025" step=".025"></label><label id="label-bar-height" hidden>Bar height · mm<input id="label-height" type="number" value="12" min="1" max="100"></label></div>
    <svg id="label-proof" role="img" aria-label="First label preview" style="display:block;width:100%;height:110px;background:white;margin:8px 0"></svg>
    <p class="hint" id="label-summary" role="status"></p>
    <button type="button" id="label-add">Insert labels</button>`;
  $('design-panel-text').append(panel);
  const el=name=>$('label-'+name), val=name=>el(name).valueAsNumber;
  const values=()=>sequence(el('pattern').value,val('count'),val('start'),val('increment'),val('digits'));
  function preview() {
    const kind=el('kind').value;
    el('symbol-settings').hidden=kind==='text';el('bar-height').hidden=kind!=='barcode';
    try {
      const items=values();
      if(kind==='barcode')items.forEach(code39);
      const proof=el('proof');
      proof.replaceChildren();
      const path=document.createElementNS('http://www.w3.org/2000/svg','path');
      if(kind==='text'){
        const first=KerfTextTools.textObject(items[0],0,0);
        const [x,y,right,top]=bounds(first.paths),margin=Math.max((top-y)*.12,.25);
        proof.setAttribute('viewBox',[x-margin,-top-margin,right-x+2*margin,top-y+2*margin].join(' '));
        path.setAttribute('d',first.paths.map(line=>'M'+line.map(([px,py])=>px+','+(-py)).join('L')).join(''));
        path.setAttribute('fill','none');path.setAttribute('stroke','#173842');path.setAttribute('stroke-width','1.2');path.setAttribute('vector-effect','non-scaling-stroke');
      }else{
        const {rows,quietX,quietY}=kind==='qr'?qr(items[0]):code39(items[0]);
        const unit=val('unit'),bar=val('height');
        if(!Number.isFinite(unit)||unit<.1||unit>5||!Number.isFinite(bar)||bar<1||bar>100)throw Error('Check module width and bar height.');
        const rowHeight=kind==='qr'?1:bar/unit;
        proof.setAttribute('viewBox',[0,0,rows[0].length+2*quietX,rows.length*rowHeight+2*quietY].join(' '));
        const commands=[];
        rows.forEach((row,y)=>{for(let x=0;x<row.length;){if(!row[x]){x++;continue;}const start=x;while(x<row.length&&row[x])x++;
          commands.push('M'+(start+quietX)+' '+(quietY+y*rowHeight)+'h'+(x-start)+'v'+rowHeight+'h'+(start-x)+'Z');
        }});
        path.setAttribute('d',commands.join(''));path.setAttribute('shape-rendering','crispEdges');path.setAttribute('fill','#14252a');
      }
      proof.append(path);
      const help=kind==='text'?'Uses the selected text font and layout.':'Use a line engraving operation. Keep the margin around each code clear.';
      el('summary').textContent=(items.length===1?items[0]:items[0]+' … '+items.at(-1))+' · '+help;
    } catch(e) { el('proof').replaceChildren();el('summary').textContent=e.message; }
  }
  $('design-panel-text').addEventListener('input',()=>{if(panel.open)preview();});
  panel.addEventListener('toggle',()=>{if(panel.open)preview();});preview();
  el('add').onclick=task(()=>{
    const kind=el('kind').value, cols=val('columns'), gap=val('gap');
    const x0=$('shapeX').valueAsNumber, y0=$('shapeY').valueAsNumber;
    if(!Number.isInteger(cols)||cols<1||cols>100||!Number.isFinite(gap)||gap<0||gap>200||![x0,y0].every(v=>Number.isFinite(v)&&v>=0))throw Error('Check grid columns, spacing and Design X/Y.');
    let pathCount=0,vertices=0,x=x0,y=y0,rowHeight=0;
    for(const o of objects){pathCount+=o.paths.length;for(const p of o.paths)vertices+=p.length;}
    const batch=[];
    for(const [index,value] of values().entries()){
      if(index>0&&index%cols===0){x=x0;y+=rowHeight+gap;rowHeight=0;}
      let item,width,height;
      if(kind==='text'){
        item=KerfTextTools.textObject(value,x,y);
        const b=bounds(item.paths);width=b[2]-b[0];height=b[3]-b[1];
      }else{
        const result=engrave(kind==='qr'?qr(value):code39(value),val('unit'),val('spacing'),val('height'),x,y,3000-pathCount);
        ({width,height}=result);
        item={name:(kind==='qr'?'QR · ':'Code 39 · ')+value,paths:result.paths,operationSettings:{operation:'line'}};
      }
      if(x+width>$('width').valueAsNumber+1e-8||y+height>$('height').valueAsNumber+1e-8)throw Error('The labels and their margins do not fit on the work area.');
      pathCount+=item.paths.length;for(const p of item.paths)vertices+=p.length;
      if(pathCount>3000||vertices>100000)throw Error('The series exceeds the project path or point limit. Use a smaller batch.');
      item.designLayerId=CNCDesignLayers.activeId();
      batch.push(item);x+=width+gap;rowHeight=Math.max(rowHeight,height);
    }
    objects.push(...batch);selected=objects.length-1;invalidate();draw();
    message('Inserted '+batch.length+' label'+(batch.length===1?'':'s')+'.');
  });
})(globalThis);
