const test=require('node:test'),assert=require('node:assert/strict');
const {estimate}=require('../studio/static/business.js');
const base={quantity:2,material:10,waste:10,minutes:30,timeFactor:1,extraMinutes:0,machineRate:60,setupMinutes:30,finishMinutes:15,laborRate:40,tooling:2,other:4,margin:20,tax:10};
test('quantity, order setup, gross margin and tax',()=>{const r=estimate(base);assert.equal(r.material,22);assert.equal(r.machine,60);assert.equal(r.labor,40);assert.equal(r.cost,130);assert.equal(r.subtotal,162.5);assert.equal(r.tax,16.25);assert.equal(r.total,178.75);});
test('setup charged once, time allowance per unit',()=>{const r=estimate({...base,quantity:4,timeFactor:2,extraMinutes:10});assert.equal(r.estimatedMachineMinutes,280);assert.equal(r.labor,60);});
test('reject invalid or unbounded prices',()=>{for(const d of [{quantity:1.5},{margin:100},{tax:-1},{minutes:NaN},{timeFactor:0}])assert.throws(()=>estimate({...base,...d}));});
