'use strict';
(()=>{
 const panel=document.createElement('details');panel.id='toolChangesPanel';
 panel.innerHTML='<summary>Manual tool changes</summary><label>Tool number<input id="manualToolNumber" type="number" min="1" max="999" step="1" value="1"></label><label>Tool name<input id="manualToolName" maxlength="80" value="Tool 1"></label><label>Tool shape<select id="manualToolShape"><option value="flat">Flat end mill</option><option value="ball">Ball end mill</option><option value="v">V bit</option><option value="drill">Drill bit</option></select></label><button id="assignManualTool">Assign tool and current settings to selected object</button><button id="generateToolPlan">Generate separate stage files</button><button id="downloadToolPlan" disabled>Download tool-change plan JSON</button><p class="hint">Router mode only. Set cutter diameter and cutting settings in CAM, then assign a tool to every object. Each stage ends retracted with spindle off. Change tools manually, set G54 Z zero for the current tool tip (desktop exports cancel tool-length offsets), check clearance, then start the next file yourself. No automatic continuation or M6 is sent.</p><div id="manualToolStages"></div>';
 document.querySelector('aside').append(panel);
 let plan=null,snapshot=null;
 function current(){if(!plan||snapshot!==JSON.stringify(machiningProject()))throw Error('The design or operation settings changed. Generate a new tool-change plan.');return plan;}
 $('assignManualTool').onclick=task(()=>{
   if($('mode').value!=='router')throw Error('Manual tool changes require router mode');
   const id=Number($('manualToolNumber').value),name=$('manualToolName').value.trim();
   if(!Number.isInteger(id)||id<1||id>999||!name||name.length>80||/[\x00-\x1f]/.test(name))throw Error('Enter tool number 1–999 and a name of 1–80 characters');
   const obj=selectedObject();
   obj.operationSettings={...obj.operationSettings,...Object.fromEntries(fields.map(key=>[key,['mode','protocol','operation','dwell_units','lead_shape','laser_pattern','router_entry'].includes(key)?$(key).value:Number($(key).value)])),tool_id:id,tool_name:name,tool_shape:$('manualToolShape').value};
   plan=null;snapshot=null;$('downloadToolPlan').disabled=true;$('manualToolStages').replaceChildren();
   invalidate();draw();message(`Assigned tool ${id} · ${name}. Assign tools to all active objects before generating stages.`);
 });
 $('generateToolPlan').onclick=task(async()=>{
   const p=machiningProject();if(p.mode!=='router')throw Error('Manual tool changes require router mode');
   const before=JSON.stringify(p),result=await api('tool-changes',{project:p});
   if(before!==JSON.stringify(machiningProject()))throw Error('Design changed while generating. Generate the plan again.');
   if(!Array.isArray(result.stages)||!result.manual_only)throw Error('Server returned an invalid manual tool-change plan');
   plan=result;snapshot=before;$('manualToolStages').replaceChildren();
   for(const stage of plan.stages){
     const row=document.createElement('div'),heading=document.createElement('p'),button=document.createElement('button');
     heading.textContent=`Stage ${stage.stage}: tool ${stage.tool.tool_id} · ${stage.tool.tool_name} · ${stage.tool.diameter} mm ${stage.tool.shape} · operations ${stage.operation_indices.join(', ')}`;
     button.textContent=`Download stage ${stage.stage} G-code`;
     button.onclick=task(()=>{current();download(stage.filename,stage.lines.join('\n')+'\n');});
     row.append(heading,button);$('manualToolStages').append(row);
   }
   $('downloadToolPlan').disabled=false;
   message(`Prepared ${plan.stages.length} independent stage files and ${plan.tool_changes} manual tool changes. Review each stage and re-establish Z after changing tools.`);
 });
 $('downloadToolPlan').onclick=task(()=>download('manual-tool-change-plan.json',JSON.stringify(current(),null,2),'application/json'));
})();
