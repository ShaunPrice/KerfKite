const { test } = require('node:test');
const assert = require('node:assert/strict');
const { encode } = require('../../studio/static/stock-wire.js');

test('stock preview v1 uses little-endian millimetres and keeps start depth', () => {
  const body=Buffer.from(encode({segments:[[0,1,2,3,-1,null,-.25],[2,3,4,5,-.5]],
    width:8,height:7,resolution:16,tool_shape:'ball',tool_diameter:1.5}));
  assert.equal(body.length,76+112);
  assert.equal(body.toString('ascii',0,8),'KKSTOCK1');
  assert.deepEqual([8,12,16].map(i=>body.readUInt32LE(i)),[2,16,1]);
  assert.deepEqual(Array.from({length:7},(_,i)=>body.readDoubleLE(20+8*i)),[0,0,8,7,1.5,60,10]);
  assert.deepEqual(Array.from({length:7},(_,i)=>body.readDoubleLE(76+8*i)),[0,1,2,3,-1,NaN,-.25]);
  assert.equal(body.readDoubleLE(76+56+48),-.5);
  assert.throws(()=>encode({segments:[[1,1,null,3,-1]]}),/finite/);
  assert.throws(()=>encode({segments:new Array(200001)}),/segment count/);
});
