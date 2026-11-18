'use strict';
/* Portable project documents are the source of truth; IndexedDB is a local library. */
(()=>{
 const FORMAT='pi-cnc-studio/1', MAX_BYTES=4000000, REVISIONS=5;
 window.projectExtras=window.projectExtras||{};
 let namespace=null, dbPromise=null, metadata={name:'Untitled project'}, loading=false, timer=null, baseline='', dirty=false;
 Object.defineProperty(window,'projectMetadata',{configurable:true,get:()=>({...metadata,name:name()})});
 const panel=document.createElement('details');panel.id='projectManager';
 panel.innerHTML='<summary>Projects &amp; recovery</summary><label>Project name<input id="projectName" maxlength="100" value="Untitled project"></label><div class="buttons"><button id="projectSaveLibrary">Save to this browser</button><button id="projectDownload">Download project</button><button id="projectNew">New project</button><button id="projectDuplicate">Save as a copy</button></div><label>Find projects<input id="projectSearch" type="search" placeholder="Search by name"></label><div id="projectList"></div><button id="projectRecover">Recover latest draft</button><p id="projectLibraryStatus" class="hint">Opening local project library…</p><p class="hint">This library stays in this browser profile. Download a project for backup or another computer. Hosted accounts have separate local libraries; use server files for remote storage.</p>';
 document.querySelector('main aside').prepend(panel);
 const status=t=>{$('projectLibraryStatus').textContent=t;};
 const clone=v=>JSON.parse(JSON.stringify(v));
 const id=()=>crypto.randomUUID?crypto.randomUUID():Array.from(crypto.getRandomValues(new Uint8Array(16)),v=>v.toString(16).padStart(2,'0')).join('');
 function name(){return $('projectName').value.trim().slice(0,100)||'Untitled project';}
 function documentData(){return {format:FORMAT,settings:project(),objects:clone(objects),metadata:{...metadata,name:name()},extras:clone(window.projectExtras||{})};}
 function fingerprint(d=documentData()){return JSON.stringify({settings:d.settings,objects:d.objects,name:d.metadata.name,extras:d.extras});}
 function encoded(d){const text=JSON.stringify(d,null,2);if(new TextEncoder().encode(text).length>MAX_BYTES)throw Error('Project exceeds the 4 MB portable import limit. Split the artwork into smaller projects before saving.');return text;}
 function projectName(document){
   const name=document?.metadata?.name;
   return typeof name==='string'&&name.trim()?name.trim().slice(0,100):'Untitled project';
 }
 function savedTime(value){
   if(typeof value==='string')return value;
   if(typeof value==='number'||value instanceof Date){
     const time=new Date(value);if(Number.isFinite(time.getTime()))return time.toISOString();
   }
   return '';
 }
 function timeLabel(value){
   const date=new Date(value||NaN);
   return Number.isFinite(date.getTime())?date.toLocaleString():'Save date unavailable';
 }
 function revisions(row){
   return Array.isArray(row?.revisions)?row.revisions.filter(r=>r?.document && typeof r.document==='object'): [];
 }
 function summary(row){
   // Draft keys are addressable for recovery status and omitted from the namespace search index.
   if(row.draft)return {key:row.key,saved:savedTime(row.saved)};
   return {key:row.key,namespace:row.namespace,name:projectName(row.document),saved:savedTime(row.saved),
     revisions:revisions(row).map(r=>savedTime(r.saved))};
 }
 function connection(){
   if(dbPromise)return dbPromise;
   dbPromise=new Promise((resolve,reject)=>{
     if(!window.indexedDB){reject(Error('IndexedDB is unavailable'));return;}
     const req=indexedDB.open('pi-cnc-project-library',2);
     req.onupgradeneeded=event=>{
       const db=req.result;
       const documents=event.oldVersion===0?db.createObjectStore('projects',{keyPath:'key'}):req.transaction.objectStore('projects');
       const summaries=db.createObjectStore('summaries',{keyPath:'key'});
       summaries.createIndex('namespace','namespace');
       if(event.oldVersion===1){
         // Upgrade incrementally so even a library full of revisions needs only one record in JS.
         const cursor=documents.openCursor();
         cursor.onsuccess=()=>{const current=cursor.result;if(!current)return;
           summaries.put(summary(current.value));
           current.continue();
         };
       }
     };
     req.onsuccess=()=>{
       const db=req.result;
       db.onversionchange=()=>{db.close();dbPromise=null;};
       resolve(db);
     };
     req.onerror=()=>{dbPromise=null;reject(req.error);};
     req.onblocked=()=>status('Close older Studio tabs to finish upgrading your project library.');
   });
   return dbPromise;
 }
 async function transaction(stores,mode,fn){
   if(!namespace)throw Error('Project library is not ready');
   const db=await connection();
   return new Promise((resolve,reject)=>{
     const tx=db.transaction(stores,mode);let result;
     try{result=fn(...stores.map(name=>tx.objectStore(name)));}
     catch(e){tx.abort();reject(e);return;}
     tx.oncomplete=()=>resolve(result?.result);
     tx.onabort=()=>reject(tx.error||Error('Project storage failed'));
   });
 }
 const key=projectId=>namespace+':'+projectId;
 const get=projectId=>transaction(['projects'],'readonly',s=>s.get(key(projectId)));
 function failure(e){status('Local storage unavailable: '+e.message+'. Download project still works.');message('Could not save locally. Download your project to keep a portable copy.',true);}
 let refreshVersion=0;
 async function refresh(){
   const ticket=++refreshVersion;
   const all=await transaction(['summaries'],'readonly',s=>s.index('namespace').getAll(namespace));
   if(ticket!==refreshVersion)return;
   const query=$('projectSearch').value.toLowerCase(), list=$('projectList');
   list.replaceChildren();
   const rows=all.map(row=>({...row,name:typeof row.name==='string'&&row.name.trim()?row.name:'Untitled project',
     saved:savedTime(row.saved),revisions:Array.isArray(row.revisions)?row.revisions.map(savedTime):[]}));
   for(const row of rows.filter(r=>r.name.toLowerCase().includes(query)).sort((a,b)=>b.saved.localeCompare(a.saved))){
     const line=document.createElement('p'),label=document.createElement('span');
     label.textContent=row.name+' · '+timeLabel(row.saved);line.append(label);
     function button(title,action){const b=document.createElement('button');b.textContent=title;b.onclick=task(action);line.append(b);}
     async function openRevision(index){
       const stored=await transaction(['projects'],'readonly',s=>s.get(row.key));
       const revision=index===undefined?null:revisions(stored)[index];
       const doc=index===undefined?stored?.document:
         (savedTime(stored?.saved)===row.saved && savedTime(revision?.saved)===row.revisions[index]?revision?.document:null);
       if(!stored || (index!==undefined&&!doc)){await refresh();throw Error('This save is no longer in the library.');}
       await openDocument(doc,row.key);
     }
     button('Open',()=>openRevision());
     button('Delete',async()=>{
       if(!confirm('Delete “'+row.name+'” and its saved revisions from this browser?'))return;
       await transaction(['projects','summaries'],'readwrite',(docs,summaries)=>{docs.delete(row.key);summaries.delete(row.key);});
       await refresh();
     });
     if(row.revisions.length){
       const versions=document.createElement('select');
       versions.setAttribute('aria-label','Saved revisions for '+row.name);
       versions.append(new Option('Choose older revision',''));
       row.revisions.forEach((stamp,i)=>versions.append(new Option(timeLabel(stamp),String(i))));
       versions.onchange=task(async()=>{if(versions.value!=='')await openRevision(Number(versions.value));versions.value='';});
       line.append(versions);
     }
     list.append(line);
   }
   if(!list.childNodes.length)list.textContent='No matching saved projects.';
 }
 function canReplace(){return !(dirty||fingerprint()!==baseline)||confirm('Replace the current design? Download or save it first if you want to keep unsaved changes.');}
 async function openDocument(d,storedKey){
   if(!d||typeof d!=='object'||Array.isArray(d))throw Error('This save has no readable project document.');
   // Supply missing portable metadata in memory. Merely opening or migrating a
   // legacy record must leave its original document and revision data untouched.
   if(typeof storedKey==='string' && storedKey.startsWith(namespace+':') &&
       (d.metadata===undefined || (d.metadata && typeof d.metadata==='object' && !Array.isArray(d.metadata)))){
     const projectId=storedKey.slice(namespace.length+1);
     d={...d,metadata:{...d.metadata,name:projectName(d),
       ...(projectId && projectId.length<=100 && projectId!=='@draft'?{id:projectId}:{})}};
   }
   await loadFile(new File([encoded(d)],'project.cncstudio.json',{type:'application/json'}),'project');
 }
 async function save(copy=false){
   clearTimeout(timer);
   const d=documentData(),savedName=d.metadata.name,projectId=copy||!metadata.id?id():metadata.id;
   const now=new Date().toISOString();
   d.metadata={...d.metadata,id:projectId,created:copy?now:(metadata.created||now),modified:now};
   if(copy)d.metadata.name=savedName+' copy';
   encoded(d);
   await transaction(['projects','summaries'],'readwrite',(docs,summaries)=>{
     const existing=docs.get(key(projectId));
     existing.onsuccess=()=>{
       const old=existing.result;
       const prior=old?[{saved:old.saved,document:old.document},...revisions(old)].slice(0,REVISIONS):[];
       const row={key:key(projectId),namespace,document:d,saved:now,revisions:prior};
       docs.put(row);summaries.put(summary(row));
     };
   });
   metadata=d.metadata;if(name()===savedName)$('projectName').value=metadata.name;
   baseline=fingerprint(d);dirty=fingerprint()!==baseline;if(dirty)changed();
   status('Saved “'+metadata.name+'” in this browser. Up to five earlier saves are retained.');await refresh();
 }
 async function draft(){
   if(!namespace||loading)return;
   try{
     const d=documentData();
     if(fingerprint(d)===baseline)return;
     encoded(d);
     const row={key:key('@draft'),namespace,draft:true,document:d,saved:new Date().toISOString()};
     await transaction(['projects','summaries'],'readwrite',(docs,summaries)=>{docs.put(row);summaries.put(summary(row));});
     status('Recovery draft saved · '+new Date().toLocaleTimeString());
   }catch(e){failure(e);}
 }
 function changed(){if(loading||!namespace)return;dirty=true;clearTimeout(timer);timer=setTimeout(draft,1500);}
 const originalInvalidate=invalidate;invalidate=function(...args){const r=originalInvalidate(...args);changed();return r;};
 const originalLoad=loadFile;async function loadProjectFile(file,kind,confirmed=false){if(kind!=='project')return originalLoad(file,kind);if(!file)return;if(file.size>MAX_BYTES)throw Error('File exceeds 4 MB');if(!confirmed&&!canReplace())return;const d=JSON.parse(await file.text());if(d.metadata!==undefined&&(!d.metadata||typeof d.metadata!=='object'||Array.isArray(d.metadata)))throw Error('Invalid project metadata');if(d.extras!==undefined&&(!d.extras||typeof d.extras!=='object'||Array.isArray(d.extras)))throw Error('Invalid project extras');loading=true;try{await originalLoad(file,kind);metadata={...clone(d.metadata||{}),name:typeof d.metadata?.name==='string'?d.metadata.name.slice(0,100):file.name.replace(/\.cncstudio\.json$/i,''),...(typeof d.metadata?.id==='string'&&d.metadata.id!=='@draft'?{id:d.metadata.id.slice(0,100)}:{}),...(typeof d.metadata?.created==='string'?{created:d.metadata.created}:{})};if(metadata.id==='@draft'||typeof metadata.id!=='string')delete metadata.id;window.projectExtras=clone(d.extras||{});$('projectName').value=metadata.name;baseline=fingerprint();dirty=false;clearTimeout(timer);window.dispatchEvent(new CustomEvent('cnc-project-loaded',{detail:documentData()}));}finally{loading=false;}};
 loadFile=(file,kind)=>loadProjectFile(file,kind);
 const originalArtifact=artifact;artifact=function(kind){if(kind!=='project')return originalArtifact(kind);return {name:name().replace(/[^a-zA-Z0-9._ -]/g,'_')+'.cncstudio.json',content:encoded(documentData())};};
 function portable(){const f=artifact('project');download(f.name,f.content,'application/json');status('Project downloaded. Keep this file as a portable backup.');}
 $('saveProject').onclick=task(portable);$('projectDownload').onclick=task(portable);
 $('projectSaveLibrary').onclick=task(async()=>{try{await save();}catch(e){failure(e);}});
 $('projectDuplicate').onclick=task(async()=>{try{await save(true);}catch(e){failure(e);}});
 $('projectName').oninput=changed;$('projectSearch').oninput=task(refresh);
 $('projectRecover').onclick=task(async()=>{const row=await get('@draft');if(!row)throw Error('No recovery draft is stored for this account in this browser');await openDocument(row.document);});
 function newProject(){loading=true;try{objects=[];selected=-1;metadata={name:'Untitled project'};window.projectExtras={};$('projectName').value=metadata.name;invalidate();draw();clearTimeout(timer);baseline=fingerprint();dirty=false;window.dispatchEvent(new CustomEvent('cnc-project-loaded',{detail:documentData()}));}finally{loading=false;}}
 $('projectNew').onclick=task(()=>{if(canReplace())newProject();});
 document.addEventListener('keydown',e=>{if((e.ctrlKey||e.metaKey)&&e.key.toLowerCase()==='s'){e.preventDefault();save().catch(failure);}});
 window.addEventListener('beforeunload',e=>{if(dirty){e.preventDefault();e.returnValue='';}});
 window.addEventListener('cnc-project-changed',changed);
 window.CNCProjects={document:documentData,fingerprint,newProject,openConfirmed:file=>loadProjectFile(file,'project',true),save,refresh,changed,download:portable};
 window.studioReady.then(async session=>{namespace=session.web_mode?'account:'+session.username:'local';baseline=fingerprint();try{await refresh();const recovery=await transaction(['summaries'],'readonly',s=>s.get(key('@draft')));status(recovery?'A recovery draft is available from '+timeLabel(recovery.saved)+'. Use Recover latest draft to inspect it.':'Local project library ready. Download files for portable backups.');}catch(e){failure(e);}}).catch(e=>status('Project library unavailable until the editor starts: '+e.message));
})();
