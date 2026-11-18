const test=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs'),vm=require('node:vm');
const apiSource=fs.readFileSync(require('node:path').resolve(__dirname,'../../studio/static/app.js'),'utf8').split('\n').find(line=>line.startsWith('async function api('));
test('old server cannot silently drop holding tabs in preview, rotary or tool stages',async()=>{
 let calls=0;const context={offline:false,window:{studioSession:{advanced_cam:true,depth_paths:true}},token:'test',fetch:async()=>{calls++;return {ok:true,json:async()=>({})};},JSON};vm.createContext(context);vm.runInContext(apiSource,context);
 const project={mode:'router',operations:[{tabs:{width:5,height:1,count:3}}]};
 for(const path of ['generate','rotary','tool-changes'])await assert.rejects(context.api(path,path==='generate'?project:{project}),/cannot preserve/);
 assert.equal(calls,0);context.window.studioSession.holding_tabs=true;await context.api('generate',project);assert.equal(calls,1);
});
