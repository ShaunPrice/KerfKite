/* A local Studio session owns the separately paired monitor and CNC controller. */
(() => {
  'use strict';
  const panel = document.createElement('details');
  panel.hidden = true;
  panel.innerHTML = `<summary>Phone pendant</summary>
    <p class="hint">Connect your phone over Wi-Fi, USB tethering or Bluetooth tethering. Use this server’s address on that connection. Each phone command goes through Studio.</p>
    <label id="phone-plaintext-row" hidden><input type="checkbox" id="phone-plaintext"> Unencrypted pairing, for Pocket 0.4.1 and earlier or a phone browser</label>
    <button type="button" id="phone-enable">Enable phone pairing</button>
    <div id="phone-pair-fields" hidden>
      <label>Server address on the phone’s network<input id="phone-host" list="phone-hosts" placeholder="Server IP or hostname" autocomplete="off"></label><datalist id="phone-hosts"></datalist>
      <label>Private pairing link<input id="phone-link" readonly></label>
      <div id="phone-qr" role="img" aria-label="Pairing QR code for Pocket" style="width:min(100%,240px);margin:8px 0" hidden></div>
      <p id="phone-qr-hint" class="hint" hidden>In Pocket 0.5.0 or later, open CNC pendant and choose Scan QR code. The code carries this session’s pairing secret: keep it out of view of other cameras, and disable pairing to revoke it.</p>
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
    el('plaintext-row').hidden=enabled || !state?.can_encrypt || state?.encryption==='certificate';
    if(state && !busy)el('plaintext').checked=!!state.plaintext;
    el('pair-fields').hidden=!enabled;
    let link='';
    if(enabled && /^[\w.:%\[\]-]+$/.test(host)){
      try {
        const name=host.includes(':') && !host.startsWith('[') ? '['+host+']' : host;
        const url=new URL(state.scheme+'://'+name+':'+state.port+'/');
        url.hash=state.fragment||new URLSearchParams({token:state.token}).toString();
        link=url.href;
      } catch {}
    }
    el('link').value=link;
    let code='';
    if(link){try{code=KerfPairingQr.svg(link);}catch{}}
    el('qr').innerHTML=code;
    el('qr').hidden=el('qr-hint').hidden=!code;
    el('copy').disabled=!link;
    el('pair-status').textContent=enabled ? (state.encryption==='pinned'?'Encrypted pairing enabled. Pocket 0.5.0 or later accepts only this session’s certificate, identified in the link. A phone browser will warn about the certificate.':state.scheme==='https'?'HTTPS pairing enabled.':'Pairing uses HTTP. Use a trusted local connection, install the Studio web extra for encrypted pairing, or set a monitor TLS certificate on the server.') : 'Pairing is off.';
  }
  async function change(action){
    if(busy)return;
    busy=true;
    try{
      state=await api('phone-monitor',action==='enable' && !el('plaintext-row').hidden ? {action,plaintext:el('plaintext').checked} : {action});
      if(!el('host').value && state.hosts?.length)el('host').value=state.hosts[0];
      // Every address of this workstation, the likeliest first: the phone must be able to reach the one chosen.
      el('hosts').replaceChildren(...(state.hosts||[]).map(host=>Object.assign(document.createElement('option'),{value:host})));
      display();
    }catch(error){el('pair-status').textContent=error.message;}
    finally{busy=false;}
  }
  el('enable').onclick=()=>change('enable');
  el('disable').onclick=()=>change('disable');
  el('host').oninput=display;
  el('copy').onclick=async()=>{
    try { await navigator.clipboard.writeText(el('link').value); el('pair-status').textContent='Link copied. Paste it into CNC pendant on your phone, or scan the QR code there.'; }
    catch {el('link').focus();el('link').select();el('pair-status').textContent='Copy the selected link into CNC pendant on your phone.';}
  };
  panel.addEventListener('toggle',()=>{if(panel.open)change('status');});
  window.studioReady.then(session=>{if(!offline && !session.web_mode)panel.hidden=false;}).catch(()=>{});
})();
