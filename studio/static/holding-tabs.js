'use strict';
(()=>{
 const panel=document.createElement('details');panel.id='holdingTabsPanel';
 panel.innerHTML='<summary>Holding tabs · router contours</summary><p class="hint">These draft settings apply to the selected operation only when you click Assign. Stock thickness also updates the project default on assignment.</p><label>Stock thickness for selected operation · mm<input id="tabStockThickness" type="number" min=".01" max="50" step=".1"></label><label>Nominal tab width along contour · mm<input id="tabWidth" type="number" min=".01" max="100" step=".1" value="5"></label><label>Tab height above stock bottom · mm<input id="tabHeight" type="number" min=".01" max="50" step=".1" value="2"></label><label>Placement<select id="tabPlacement"><option value="count">Evenly spaced</option><option value="positions">Explicit contour positions</option></select></label><label id="tabCountLabel">Tabs per closed contour<input id="tabCount" type="number" min="1" max="100" step="1" value="4"></label><label id="tabPositionsLabel" hidden>Positions · % around contour, separated by commas<input id="tabPositions" value="12.5,37.5,62.5,87.5" placeholder="0 to less than 100"></label><button id="assignHoldingTabs">Assign tabs and current CAM settings to selected object</button><button id="removeHoldingTabs">Remove tabs from selected object</button><p id="holdingTabsStatus" class="hint">Set stock thickness and a cut depth reaching its bottom. Flat end mill only; closed line/inside/outside contours. Tabs are positioned on the compensated path. One cutter diameter of plateau padding is added. Inspect corners and nearby cuts; tabs do not replace clamping. Generate a fresh 3D preview to inspect retained tab height.</p>';
 document.querySelector('aside.operations').append(panel);
 function syncStock(){ $('tabStockThickness').value=$('stock_thickness').value; }
 syncStock();
 window.addEventListener('cnc-project-loaded',syncStock);
 $('tabPlacement').onchange=()=>{const explicit=$('tabPlacement').value==='positions';$('tabCountLabel').hidden=explicit;$('tabPositionsLabel').hidden=!explicit;};
 function configuration(){
   const width=Number($('tabWidth').value),height=Number($('tabHeight').value),thickness=Number($('tabStockThickness').value),depth=Number($('depth').value);
   if(!Number.isFinite(thickness)||!Number.isFinite(depth)||!Number.isFinite(width)||width<.01||width>100||!Number.isFinite(height)||height<.01||height>=thickness||thickness>50||depth<thickness)throw Error('Enter positive tab width and height; height must be below stock thickness, and cut depth must reach the stock bottom (maximum 50 mm).');
   if($('tabPlacement').value==='positions'){
     const text=$('tabPositions').value.trim();if(!text)throw Error('Enter contour positions as percentages');
     const positions=text.split(',').map(value=>{if(!value.trim())throw Error('Empty tab position');return Number(value.trim())/100;});
     if(positions.length>100||positions.some(value=>!Number.isFinite(value)||value<0||value>=1))throw Error('Use 1–100 contour positions from 0% to less than 100%');
     return {width,height,positions};
   }
   const count=Number($('tabCount').value);if(!Number.isInteger(count)||count<1||count>100)throw Error('Use 1–100 tabs per contour');
   return {width,height,count};
 }
 $('assignHoldingTabs').onclick=task(()=>{
   const obj=selectedObject();
   if($('mode').value!=='router'||!['line','inside','outside'].includes($('operation').value))throw Error('Holding tabs require router line, inside or outside contours');
   if(obj.depths||obj.operationSettings?.tool_shape&&obj.operationSettings.tool_shape!=='flat')throw Error('Holding tabs cannot combine with relief/depth paths or non-flat tools');
   if(obj.paths.some(path=>path.length<4||Math.hypot(path[0][0]-path.at(-1)[0],path[0][1]-path.at(-1)[1])>1e-6))throw Error('Every contour in the selected object must be closed');
   const tabs=configuration();
   window.recordHistory?.();
   $('stock_thickness').value=$('tabStockThickness').value;
   obj.operationSettings={...obj.operationSettings,...Object.fromEntries(fields.map(key=>[key,['mode','protocol','operation','dwell_units'].includes(key)?$(key).value:Number($(key).value)])),tabs};
   invalidate();draw();message('Holding tabs assigned to the selected object. Generate a new preview; placement overlaps and cutter-path geometry are validated during CAM.');
 });
 $('removeHoldingTabs').onclick=task(()=>{const obj=selectedObject();window.recordHistory?.();if(obj.operationSettings)delete obj.operationSettings.tabs;invalidate();draw();message('Holding tabs removed from selected object. Generate a new preview before running.');});
})();
