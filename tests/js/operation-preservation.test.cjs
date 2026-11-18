const test=require('node:test');
const assert=require('node:assert/strict');
const vm=require('node:vm');
const fs=require('node:fs');
const path=require('node:path');

function setup(){
 const elements=new Map(),calls=[];
 const $=id=>{if(!elements.has(id))elements.set(id,{value:'',disabled:false,replaceChildren(){},append(){}});return elements.get(id);};
 const values={mode:'router',protocol:'grbl',operation:'outside',dwell_units:'milliseconds',width:'100',height:'100',feed:'600',power:'1000',depth:'10',stock_thickness:'10',tool_diameter:'2'};
 for(const[k,v]of Object.entries(values))$(k).value=v;
 for(const[k,v]of Object.entries({manualToolNumber:'1',manualToolName:'Cutter',manualToolShape:'flat'}))$(k).value=v;
 const obj={name:'Tabbed cutout',paths:[[[10,10],[30,10],[30,30],[10,30],[10,10]]],operationSettings:{tabs:{width:4,height:2,count:4},stock_thickness:10,tool_id:1,tool_name:'Original cutter',tool_shape:'flat',tool_diameter:2}};
 const context=vm.createContext({$,objects:[obj],fields:Object.keys(values),selectedObject:()=>obj,
  project:()=>({...Object.fromEntries(Object.keys(values).map(k=>[k,['mode','protocol','operation','dwell_units'].includes(k)?$(k).value:Number($(k).value)])),paths:obj.paths}),
  task:fn=>fn,invalidate(){},draw(){},message(){},download(){},prompt(){return null;},
  window:{studioReady:{then(){}}},localStorage:{getItem(){return null;},setItem(){}},
  document:{createElement(){return {innerHTML:'',append(){}};},querySelector(){return {append(){}};}},
  Option:function(label,value){this.label=label;this.value=value;},
  api:async(route,payload)=>{calls.push({route,payload});return {manual_only:true,stages:[],tool_changes:0};}
 });
 for(const file of ['layers.js','tool-changes.js'])vm.runInContext(fs.readFileSync(path.join(__dirname,'../../studio/static',file),'utf8'),context,{filename:file});
 return {$,obj,context,calls,project:()=>JSON.parse(vm.runInContext('JSON.stringify(project())',context))};
}

test('tabs survive feed reassignment, manual tool assignment and stage request',async()=>{
 const f=setup(),tabs=structuredClone(f.obj.operationSettings.tabs);
 f.$('feed').value='250';f.$('assignOperation').onclick();
 assert.deepEqual(f.obj.operationSettings.tabs,tabs);assert.equal(f.obj.operationSettings.feed,250);
 assert.equal(f.obj.operationSettings.tool_id,1);assert.equal(f.obj.operationSettings.tool_name,'Original cutter');
 f.$('manualToolNumber').value='3';f.$('manualToolName').value='Finishing tool';f.$('assignManualTool').onclick();
 assert.deepEqual(f.obj.operationSettings.tabs,tabs);assert.equal(f.obj.operationSettings.tool_id,3);
 assert.equal(f.obj.operationSettings.tool_shape,'flat');assert.equal(f.project().operations[0].stock_thickness,10);
 await f.$('generateToolPlan').onclick();
 assert.equal(f.calls.length,1);assert.equal(f.calls[0].route,'tool-changes');
 assert.deepEqual(f.calls[0].payload.project.operations[0].tabs,tabs);
 assert.equal(f.calls[0].payload.project.operations[0].feed,250);
});

test('global-settings reset cannot silently erase tabs',()=>{
 const f=setup();
 assert.throws(()=>f.$('clearOperation').onclick(),/Remove holding tabs explicitly/);
 assert.ok(f.obj.operationSettings.tabs);assert.equal(f.obj.operationSettings.stock_thickness,10);
 delete f.obj.operationSettings.tabs;f.$('clearOperation').onclick();
 assert.equal(f.obj.operationSettings,undefined);
});

test('incompatible new mode or tool retains tabs so CAM can reject it',()=>{
 const f=setup();f.$('mode').value='laser';f.$('assignOperation').onclick();
 assert.ok(f.project().operations[0].tabs);assert.equal(f.project().operations[0].mode,'laser');
 f.$('mode').value='router';f.$('manualToolShape').value='ball';f.$('assignManualTool').onclick();
 assert.equal(f.project().operations[0].tool_shape,'ball');assert.ok(f.project().operations[0].tabs);
});

test('explicitly disabled tabs do not block clearing operation settings',()=>{
 for(const disabled of [null,false,undefined]){const f=setup();f.obj.operationSettings.tabs=disabled;f.$('clearOperation').onclick();assert.equal(f.obj.operationSettings,undefined);}
});

test('dwell units survive operation and tool assignment',()=>{const f=setup();f.$('assignOperation').onclick();assert.equal(f.obj.operationSettings.dwell_units,'milliseconds');f.$('assignManualTool').onclick();assert.equal(f.obj.operationSettings.dwell_units,'milliseconds');});
