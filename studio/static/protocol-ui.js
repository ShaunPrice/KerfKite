/* Keep profile information and export/live controls explicit. */
'use strict';
(()=>{
 function update(){
  const p=CNCProtocols.get($('protocol').value);
  $('protocolHint').textContent=p.notes;
  $('dwellUnitsLabel').hidden=!['mach3','uccnc'].includes(p.id);
  $('connect').disabled=!p.live_transport;
  $('applyHead').disabled=!p.live_transport;
  for(const b of document.querySelectorAll('[data-jog],[data-command=home],[data-command=zero],[data-command=unlock]'))b.disabled=!p.live_transport;
  $('armPhone').disabled=!p.live_transport;
  $('run').disabled=!p.live_transport||!latest.connected||latest.protocol!==p.id;
 }
 $('protocol').addEventListener('change',update);
 window.addEventListener('cnc-project-loaded',update);
 window.studioReady.then(update);
 update();
})();
