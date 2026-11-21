const test=require('node:test'),assert=require('node:assert/strict');
require('../../studio/static/protocols.js');
const {validate}=require('../../studio/static/machine-library.js');
const profile={name:' Bench router ',width:420,height:297,zTravel:60,protocol:'grbl'};
test('a profile keeps its measured work area and dialect',()=>assert.deepEqual(validate(profile),{name:'Bench router',width:420,height:297,zTravel:60,protocol:'grbl'}));
test('a profile made from a controller report remembers its machine',()=>{
  assert.equal(validate({...profile,source:'grbl|Grbl 1.1h|420|297|60'}).source,'grbl|Grbl 1.1h|420|297|60');
  for(const source of ['',42,null,'x'.repeat(121)])assert.equal('source' in validate({...profile,source}),false);
});
test('travel outside what a workspace allows is refused',()=>{
  assert.throws(()=>validate({...profile,width:2500}),/between 1 and 2,000/);
  assert.throws(()=>validate({...profile,zTravel:0}),/Z travel/);
  assert.equal(validate({...profile,zTravel:null}).zTravel,null);
});
