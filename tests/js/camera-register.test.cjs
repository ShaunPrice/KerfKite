const test=require('node:test');
const assert=require('node:assert/strict');
const reg=require('../../studio/static/camera-register.js');
const trace=require('../../studio/static/camera-trace.js');
const bed=[200,150];
const image=[[.1,.1],[.9,.12],[.85,.8],[.15,.88]];
const expected=([u,v])=>[(120*u+15*v+10)/(1+.3*u+.12*v),(-8*u-98*v+120)/(1+.3*u+.12*v)];
const pairs=image.map(p=>({image:p,work:expected(p)}));
function close(p,q){for(let i=0;i<2;i++)assert.ok(Math.abs(p[i]-q[i])<1e-9);}
test('four measured pairs recover perspective at independent locations and inverse overlay coordinates',()=>{
  const cal=reg.calibrate([pairs[2],pairs[0],pairs[1],pairs[3]],bed);
  for(const uv of [[.38,.55],[.71,.32],[.48,.2],...image]){
    close(reg.apply(cal.forward,uv),expected(uv));
    close(reg.apply(cal.inverse,expected(uv)),uv);
  }
  assert.equal(reg.inside([0,.5],cal.image),false);
});
test('registrations reject collinear, folded, duplicated or out-of-bounds landmarks',()=>{
  for(const invalid of [
    pairs.map((p,i)=>({...p,image:[.1*i,.1*i]})),
    [pairs[0],pairs[0],pairs[2],pairs[3]],
    pairs.map((p,i)=>({...p,work:pairs[[0,2,1,3][i]].work})),
    pairs.map((p,i)=>i===0?{...p,work:[210,5]}:p),
    pairs.slice(0,3)
  ])assert.throws(()=>reg.calibrate(invalid,bed));
});
test('a thresholded block traces to the measured work plane with an exact closed seam',()=>{
  const mask=new Uint8Array(100*100);for(let y=35;y<46;y++)for(let x=30;x<65;x++)mask[y*100+x]=1;
  const paths=trace.contours(mask,100,100),cal=reg.calibrate(pairs,bed);
  const mapped=reg.place(paths,[100,100],cal,bed);
  assert.equal(mapped[0].length,5);close(mapped[0][0],mapped[0][4]);
  close(mapped[0][0],expected([.3,.35]));
  assert.throws(()=>reg.place([[[0,0],[20,30]]],[100,100],cal,bed),/outside/);
});
