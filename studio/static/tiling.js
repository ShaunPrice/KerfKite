/* Tiling: a router, knife or laser job larger than the machine, cut one tile at a time. The server divides the previewed job
   into a program for each tile, each with the tile's corner as X0 Y0. A tile is exported as a file, or run on
   the connected controller through the ordinary run route with the usual confirmation. */
(()=>{
'use strict';
const host=document.querySelector('aside.operations');
if(!host||(typeof offline!=='undefined'&&offline))return;
const panel=document.createElement('details');panel.id='tilingPanel';
panel.innerHTML=`<summary>Tile an oversized job</summary>
<p class="hint">For a design larger than the machine can reach. Set the work area to the size of the whole sheet, design and preview the job, then divide it into tiles the size of the machine's travel. Cut a tile, slide the sheet along by exactly one tile against a fence or pins, set work zero at the next tile's front-left corner, and cut the next. Cuts that cross from one tile to the next meet at the tile's edge. Router, knife and laser jobs can be tiled; a laser's beam is put out wherever a cut leaves a tile.</p>
<div class="pair"><label>Tile width · mm<input id="tileWidth" type="number" min="10" max="10000" step="1" value="300"></label><label>Tile height · mm<input id="tileHeight" type="number" min="10" max="10000" step="1" value="200"></label></div>
<button id="tilePlan" type="button">Divide the previewed job into tiles</button>
<div id="tileState" class="hint" role="status">No tiles yet</div><div id="tileList"></div>`;
host.append(panel);
let plan=null,snapshot=null;
const current=()=>{
 if(!preview||previewProject!==JSON.stringify(machiningProject()))throw Error('Generate a current preview first');
 return machiningProject();
};
const fresh=()=>{if(!plan||snapshot!==JSON.stringify(machiningProject()))throw Error('The design or its settings changed. Divide the job into tiles again.');return plan;};
const name=tile=>`tile-column${tile.column+1}-row${tile.row+1}.nc`;
function show(){
 const list=$('tileList');list.replaceChildren();
 if(!plan){$('tileState').textContent='No tiles yet';return;}
 $('tileState').textContent=`${plan.tiles.length} tiles with cutting in a grid of ${plan.columns} × ${plan.rows}, each ${plan.tile_width} × ${plan.tile_height} mm. The job spans X${plan.bounds[0].toFixed(1)} to X${plan.bounds[2].toFixed(1)} and Y${plan.bounds[1].toFixed(1)} to Y${plan.bounds[3].toFixed(1)}.`;
 for(const tile of plan.tiles){
   const row=document.createElement('div');row.className='buttons';row.dataset.tile=`${tile.column},${tile.row}`;
   const label=document.createElement('span');label.textContent=`Column ${tile.column+1}, row ${tile.row+1} · from X${tile.origin[0]} Y${tile.origin[1]} · ${tile.cut_length.toFixed(0)} mm of cutting`;
   const save=document.createElement('button');save.type='button';save.textContent='Export';
   save.onclick=task(()=>{fresh();download(name(tile),tile.lines.join('\n')+'\n');});
   const run=document.createElement('button');run.type='button';run.textContent='Run this tile';
   run.onclick=task(async()=>{
     fresh();
     if(!$('confirmed').checked)throw Error('On the Control tab, tick the check that work zero, tool, mode and clearances are right for this tile');
     const result=await api('run',{project:machiningProject(),confirmed:true,tile:{width:plan.tile_width,height:plan.tile_height,column:tile.column,row:tile.row}});
     $('confirmed').checked=false;
     message(`Tile column ${tile.column+1}, row ${tile.row+1} submitted: ${result.lines} lines. Work zero must be at this tile's front-left corner.`);
   });
   row.append(label,save);if(!(typeof webMode!=='undefined'&&webMode))row.append(run);list.append(row);
 }
}
$('tilePlan').onclick=task(async()=>{
 const project=current();
 plan=null;snapshot=null;show();
 plan=await api('tile',{project,tile_width:Number($('tileWidth').value),tile_height:Number($('tileHeight').value)});
 snapshot=JSON.stringify(project);show();
 message(`${plan.tiles.length} tiles ready. Cut them in order and move the sheet by one tile between them.`);
});
window.addEventListener('cnc-project-loaded',()=>{plan=null;snapshot=null;show();});
})();
