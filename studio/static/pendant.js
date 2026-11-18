'use strict';
/* Pure input mapping also runs under Node for device-free regression tests. */
(function(root){
 const steps=[.01,.1,1,10];
 function keyboard(key){return {ArrowLeft:['X',-1],ArrowRight:['X',1],ArrowDown:['Y',-1],ArrowUp:['Y',1],PageDown:['Z',-1],PageUp:['Z',1]}[key]||null;}
 function gamepad(pad,mapping='xy',deadman=4){
  if(!pad?.connected||pad.mapping!=='standard'||!['xy','xyz'].includes(mapping)||![0,4,5].includes(deadman)||!pad.buttons?.[deadman]?.pressed)return null;
  const axes=mapping==='xyz'?[[0,'X',1],[1,'Y',-1],[3,'Z',-1]]:[[0,'X',1],[1,'Y',-1]];
  let best=null,magnitude=.6;
  for(const[index,axis,sign]of axes){const value=pad.axes?.[index];if(Number.isFinite(value)&&Math.abs(value)>magnitude){best=[axis,Math.sign(value)*sign];magnitude=Math.abs(value);}}
  return best;
 }
 function command(intent,step,feed){if(!intent||!['X','Y','Z'].includes(intent[0])||![-1,1].includes(intent[1])||!steps.includes(step)||!Number.isFinite(feed)||feed<1||feed>1000)throw Error('Choose a valid axis, step and feed (1–1000 mm/min)');return {action:'jog',axis:intent[0],distance:intent[1]*step,feed};}
 function createJogger({status,send,enabled}){let pending=false;return {get pending(){return pending;},async jog(payload,guard=()=>true){if(pending||!enabled()||!guard())return false;pending=true;try{const s=await status();if(!enabled()||!guard()||!s.connected||s.active||s.state!=='Idle')return false;await send(payload);return true;}finally{pending=false;}}};}
 const core={keyboard,gamepad,command,createJogger};if(typeof module!=='undefined'&&module.exports){module.exports=core;return;}root.CNCPendant=core;
 const panel=document.createElement('details');panel.id='pendantPanel';panel.hidden=true;
 panel.innerHTML='<summary>Manual &amp; pendant control</summary><label><input id="pendantEnabled" type="checkbox"> Enable manual keyboard / gamepad jogging</label><div class="pair"><label>Step · mm<select id="pendantStep"><option>.01</option><option>.1</option><option selected>1</option><option>10</option></select></label><label>Feed · mm/min<input id="pendantFeed" type="number" min="1" max="1000" value="300"></label></div><div class="buttons" id="pendantJogButtons"></div><label>Gamepad<select id="pendantDevice"><option value="">Disabled</option></select></label><button id="pendantRefresh">Find gamepads</button><label>Axis mapping<select id="pendantMapping"><option value="xy">Left stick: X / Y</option><option value="xyz">Left stick: X / Y; right vertical: Z</option></select></label><label>Hold-to-jog button<select id="pendantDeadman"><option value="4">Button 4 · left shoulder</option><option value="5">Button 5 · right shoulder</option><option value="0">Button 0 · primary face button</option></select></label><div class="buttons"><button id="pendantHold">Feed hold</button><button id="pendantReset">Controller reset / abort</button></div><p id="pendantStatus" class="hint">Disabled</p><p class="hint">Keyboard arrows move X/Y; Page Up/Down move Z, one step per press. Gamepad: hold the selected button with the stick centred, then deflect it for one step (maximum 1 mm). Return to centre before another step; one axis moves at a time. Check the mapping in the simulator first. Releasing a button stops new requests; already accepted motion may continue. Feed hold and reset depend on controller firmware. This is not a physical emergency stop. Dedicated XHC/USB pendant protocols are not implemented.</p>';
 document.querySelector('main aside').append(panel);
 let localSession=false,epoch=0,releaseRequired=true,neutralRequired=true,nextJog=0,frame=0;
 const note=t=>{$('pendantStatus').textContent=t;};
 function enabled(){return localSession&&$('pendantEnabled').checked&&!document.hidden&&document.hasFocus();}
 function typing(){if(document.querySelector('dialog[open]'))return true;const el=document.activeElement;return !!el&&(el.isContentEditable||['INPUT','TEXTAREA','SELECT'].includes(el.tagName));}
 function disable(reason){$('pendantEnabled').checked=false;epoch++;releaseRequired=true;neutralRequired=true;note(reason||'Disabled');}
 const jogger=createJogger({enabled,status:()=>api('status'),send:payload=>api('command',payload)});
 async function jog(intent,guard=()=>true,stepOverride=null){if(!enabled())return;const generation=epoch;try{const payload=command(intent,stepOverride??Number($('pendantStep').value),Number($('pendantFeed').value));const sent=await jogger.jog(payload,()=>generation===epoch&&guard());if(generation===epoch)note(sent?`${payload.axis} ${payload.distance} mm requested`:'Jog skipped: controller must be connected and idle');return sent;}catch(e){disable('Jog stopped: '+e.message);message(e.message,true);}}
 for(const axis of ['X','Y','Z'])for(const sign of [-1,1]){const b=document.createElement('button');b.textContent=axis+(sign<0?' −':' +');b.onclick=()=>jog([axis,sign]);$('pendantJogButtons').append(b);}
 $('pendantEnabled').onchange=()=>{epoch++;releaseRequired=true;neutralRequired=true;nextJog=0;note($('pendantEnabled').checked?'Enabled. Release the gamepad button, then hold it with the stick centred before jogging.':'Disabled');};
 document.addEventListener('keydown',event=>{const intent=keyboard(event.key);if(!intent||event.repeat||event.ctrlKey||event.metaKey||event.altKey||typing()||!enabled())return;event.preventDefault();jog(intent);});
 root.addEventListener('cnc-tab-changed',()=>{if(panel.offsetParent===null)disable('Disabled after leaving manual controls');});
 root.addEventListener('blur',()=>disable('Disabled after window lost focus'));
 document.addEventListener('visibilitychange',()=>{if(document.hidden)disable('Disabled while page is hidden');});
 root.addEventListener('gamepaddisconnected',()=>disable('Disabled after gamepad disconnected'));
 function refresh(){const selected=$('pendantDevice').value;$('pendantDevice').replaceChildren(new Option('Disabled',''));for(const pad of navigator.getGamepads?.()||[])if(pad?.connected&&pad.mapping==='standard'){const option=new Option(pad.id+' · index '+pad.index,String(pad.index));$('pendantDevice').append(option);}$('pendantDevice').value=selected;releaseRequired=true;}
 $('pendantRefresh').onclick=()=>{try{refresh();note('Only browser-standard mapped gamepads are supported. Press a button, then find gamepads again.');}catch(e){note('Gamepad access unavailable: '+e.message);}};
 root.addEventListener('gamepadconnected',refresh);
 for(const element of ['pendantDevice','pendantMapping','pendantDeadman'])$(element).onchange=()=>{disable('Mapping changed; check it, then enable jogging again');};
 async function cycle(now){frame=requestAnimationFrame(cycle);if(!enabled()||typing()||$('pendantDevice').value===''||jogger.pending||now<nextJog)return;try{const index=Number($('pendantDevice').value),pad=navigator.getGamepads?.()[index],button=Number($('pendantDeadman').value);if(!pad?.connected||pad.mapping!=='standard'){disable('Gamepad disconnected or standard mapping unavailable');return;}if(!pad.buttons?.[button]?.pressed){releaseRequired=false;return;}if(releaseRequired)return;const intent=gamepad(pad,$('pendantMapping').value,button);if(!intent){neutralRequired=false;return;}if(neutralRequired)return;neutralRequired=true;const step=Math.min(1,Number($('pendantStep').value)),feed=Number($('pendantFeed').value);nextJog=now+Math.max(250,step/feed*60000+150);await jog(intent,()=>{const current=navigator.getGamepads?.()[index];return !typing()&&current?.connected&&current.mapping==='standard'&&current.buttons?.[button]?.pressed;},step);}catch(e){disable('Gamepad stopped: '+e.message);}}
 $('pendantHold').onclick=task(async()=>{disable('Feed hold requested');await api('command',{action:'hold'});});
 $('pendantReset').onclick=task(async()=>{if(!confirm('Abort/reset the connected controller? Reconnection and re-establishing the work origin may be required.'))return;disable('Reset requested');await api('command',{action:'reset'});});
 root.studioReady.then(session=>{localSession=!session.offline&&!session.web_mode;panel.hidden=!localSession;if(localSession)frame=requestAnimationFrame(cycle);}).catch(()=>disable('Controller session unavailable'));
 root.addEventListener('pagehide',()=>{disable('Disabled');cancelAnimationFrame(frame);});
})(typeof window==='undefined'?globalThis:window);
