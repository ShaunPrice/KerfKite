/* A local Studio session owns the separately paired monitor and CNC controller. */
(() => {
  'use strict';
  const panel = document.createElement('details');
  panel.hidden = true;
  panel.innerHTML = `<summary>Phone pendant</summary>
    <p class="hint">Connect your phone over Wi-Fi, USB tethering or Bluetooth tethering. Use this server’s address on that connection. Each phone command goes through Studio.</p>
    <button type="button" id="phone-enable">Enable phone pairing</button>
    <div id="phone-pair-fields" hidden>
      <label>Server address on the phone’s network<input id="phone-host" placeholder="Server IP or hostname" autocomplete="off"></label>
      <label>Private pairing link<input id="phone-link" readonly></label>
      <div class="buttons"><button type="button" id="phone-copy">Copy pairing link</button>
      <button type="button" id="phone-disable">Disable and revoke pairing</button></div>
    </div><p id="phone-pair-status" class="hint" role="status"></p>`;
  $('machine-panel-control').prepend(panel);
  let state, busy=false;
  const el=id=>$('phone-'+id);
  function display() {
    const host=el('host').value.trim();
    const enabled=!!state?.enabled;
    el('enable').hidden=enabled;
    el('pair-fields').hidden=!enabled;
    let link='';
    if(enabled && /^[\w.:%\[\]-]+$/.test(host)){
      try {
        const name=host.includes(':') && !host.startsWith('[') ? '['+host+']' : host;
        const url=new URL(state.scheme+'://'+name+':'+state.port+'/');
        url.hash=new URLSearchParams({token:state.token}).toString();
        link=url.href;
      } catch {}
    }
    el('link').value=link;
    el('copy').disabled=!link;
    el('pair-status').textContent=enabled ? (state.scheme==='https'?'HTTPS pairing enabled.':'Pairing uses HTTP. Use a trusted local connection, or set a monitor TLS certificate on the server.') : 'Pairing is off.';
  }
  async function change(action){
    if(busy)return;
    busy=true;
    try{
      state=await api('phone-monitor',{action});
      if(!el('host').value && state.hosts?.length)el('host').value=state.hosts[0];
      display();
    }catch(error){el('pair-status').textContent=error.message;}
    finally{busy=false;}
  }
  el('enable').onclick=()=>change('enable');
  el('disable').onclick=()=>change('disable');
  el('host').oninput=display;
  el('copy').onclick=async()=>{
    try { await navigator.clipboard.writeText(el('link').value); el('pair-status').textContent='Link copied. Paste it into CNC pendant on your phone.'; }
    catch {el('link').focus();el('link').select();el('pair-status').textContent='Copy the selected link into CNC pendant on your phone.';}
  };
  panel.addEventListener('toggle',()=>{if(panel.open)change('status');});
  window.studioReady.then(session=>{if(!offline && !session.web_mode)panel.hidden=false;}).catch(()=>{});
})();
