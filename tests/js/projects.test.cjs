const test=require('node:test');
const assert=require('node:assert/strict');
const fs=require('node:fs');
const vm=require('node:vm');
const {File}=require('node:buffer');
function harness(){
 const elements=new Map(), records=new Map(), events=[], listeners=new Map(), gate={paused:false,pending:[]};
 const element=()=>({value:'',childNodes:[],append(...v){this.childNodes.push(...v);},replaceChildren(...v){this.childNodes=v;},prepend(){},setAttribute(){}});
 const $=id=>{if(!elements.has(id))elements.set(id,element());return elements.get(id);};
 $('projectName').value='Untitled project';
 const summaries=new Map();
 const database={close(){},transaction(){
   const tx={};
   tx.objectStore=name=>{
     const store=name==='summaries'?summaries:records;
     return {
       get(k){const req={result:structuredClone(store.get(k))};queueMicrotask(()=>req.onsuccess?.());return req;},
       index(field){return {getAll(value){return {result:structuredClone([...store.values()].filter(row=>row[field]===value))};}};},
       put(r){store.set(r.key,structuredClone(r));return {result:r.key};},
       delete(k){store.delete(k);return {};}
     };
   };
   tx.abort=()=>{};
   setImmediate(()=>{const complete=()=>tx.oncomplete?.();if(gate.paused)gate.pending.push(complete);else complete();});
   return tx;
 }};
 const context={$,console,File,TextEncoder,crypto:require('node:crypto').webcrypto,Option:function(text,value){return {text,value};},CustomEvent:function(type,o){return {type,detail:o.detail};},document:{createElement:element,querySelector:element,addEventListener(){}},setTimeout:()=>1,clearTimeout(){},indexedDB:{open(){const req={result:database};setImmediate(()=>req.onsuccess());return req;}},objects:[{name:'Shape',paths:[[[0,0],[10,0],[10,10],[0,0]]]}],selected:0,project:()=>({mode:'router',depth:1}),invalidate(){},draw(){},message(){},download(...args){context.downloaded=args;},artifact:kind=>({name:kind,content:'old'}),task:fn=>fn,confirm:()=>true,studioReady:Promise.resolve({web_mode:true,username:'alice'}),addEventListener:(name,fn)=>listeners.set(name,fn),dispatchEvent:e=>events.push(e),loadFile:async function(file){const d=JSON.parse(await file.text());if(d.invalid)throw Error('Invalid project geometry');context.objects=d.objects;}};
 context.window=context;vm.createContext(context);vm.runInContext(fs.readFileSync('studio/static/projects.js','utf8'),context);return {context,records,events,$,listeners,gate};
}
const settle=()=>new Promise(resolve=>setTimeout(resolve,15));
test('portable saves preserve named metadata and extras; failed loads do not replace extras',async()=>{
 const {context:c,$,events}=harness();await settle();$('projectName').value='Test gear';c.projectExtras={business:{price:12},rotary:{diameter:30}};
 const d=JSON.parse(c.artifact('project').content);assert.equal(d.metadata.name,'Test gear');assert.equal(d.extras.business.price,12);assert.equal(d.objects.length,1);
 await assert.rejects(c.loadFile(new File([JSON.stringify({...d,invalid:true,extras:{}})],'bad.json'),'project'),/Invalid project geometry/);assert.equal(c.projectExtras.business.price,12);
 await c.loadFile(new File([JSON.stringify({...d,metadata:{name:'Opened',customer:'A'},extras:{business:{price:15}}})],'good.json'),'project');assert.equal(c.projectMetadata.name,'Opened');assert.equal(c.projectMetadata.customer,'A');assert.equal(c.projectExtras.business.price,15);assert.equal(events.at(-1).type,'cnc-project-loaded');
});
test('IndexedDB saves are account scoped and revisions are bounded; duplicate creates a new record',async()=>{
 const {context:c,records,$}=harness();await settle();$('projectName').value='Part';await c.CNCProjects.save();
 for(let i=0;i<8;i++){c.objects[0].name='Revision '+i;await c.CNCProjects.save();}
 let rows=[...records.values()];assert.equal(rows.length,1);assert.equal(rows[0].namespace,'account:alice');assert.equal(rows[0].revisions.length,5);assert.equal(rows[0].document.objects[0].name,'Revision 7');
 await c.CNCProjects.save(true);rows=[...records.values()];assert.equal(rows.length,2);assert.equal(c.projectMetadata.name,'Part copy');
});
test('declining replacement preserves unsaved design and imports still enforce size limit',async()=>{
 const {context:c,$}=harness();await settle();$('projectName').value='Unsaved';c.confirm=()=>false;
 await c.loadFile(new File([JSON.stringify({format:'pi-cnc-studio/1',objects:[],settings:{}})],'empty.json'),'project');assert.equal(c.objects.length,1);
 await assert.rejects(c.loadFile(new File(['x'.repeat(4000001)],'large.json'),'project'),/4 MB/);
});

test('edits during an awaited save remain dirty and are not claimed by the stored snapshot',async()=>{
 const {context:c,records,$,listeners,gate}=harness();await settle();$('projectName').value='Saved title';
 gate.paused=true;const pending=c.CNCProjects.save();await settle();assert.equal(gate.pending.length,1);
 c.objects[0].name='Edited during save';$('projectName').value='New unsaved title';c.invalidate();
 gate.paused=false;gate.pending.splice(0).forEach(complete=>complete());await pending;
 const row=[...records.values()][0];assert.equal(row.document.objects[0].name,'Shape');assert.equal(row.document.metadata.name,'Saved title');
 assert.equal(c.objects[0].name,'Edited during save');assert.equal(c.projectMetadata.name,'New unsaved title');
 let prevented=false;listeners.get('beforeunload')({preventDefault(){prevented=true;}});assert.equal(prevented,true);
});
