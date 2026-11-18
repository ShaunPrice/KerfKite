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

const layout=[[.14,.16],[.84,.18],[.83,.83],[.16,.85],[.51,.25],[.75,.52],[.48,.75],[.24,.53],[.45,.45]];
function cameraFixture(k){
  const aspect=1.5;
  const corrected=([u,v])=>{
    const x=u-.5,y=v-.5,r2=4*(x*x*aspect*aspect+y*y)/(aspect*aspect+1);
    return [.5+x/(1+k*r2),.5+y/(1+k*r2)];
  };
  const work=p=>expected(corrected(p));
  const points=layout.map(uv=>({image:uv,work:work(uv)}));
  return {points,work,options:{aspect,radial:true,tolerance:.1}};
}
test('lens coefficient and homography recover held-out positions and inverse pixels for both radial signs',()=>{
  for(const coeff of [-.32, .24, 0]){
    const f=cameraFixture(coeff), cal=reg.calibrate(f.points,bed,f.options);
    assert.ok(Math.abs(cal.k-coeff)<1e-7);
    assert.ok(cal.rms<1e-6 && cal.worst<1e-6);
    for(const uv of [[.28,.61],[.65,.4],[.73,.72]]){
      const xy=f.work(uv),mapped=reg.toWork(cal,uv),back=reg.toImage(cal,xy);
      assert.ok(Math.hypot(mapped[0]-xy[0],mapped[1]-xy[1])<1e-6);
      assert.ok(Math.hypot(back[0]-uv[0],back[1]-uv[1])<1e-6);
    }
  }
});
test('radial registration rejects repeated targets and reports excessive residuals in millimetres',()=>{
  const {points,options}=cameraFixture(-.2);
  assert.throws(()=>reg.calibrate([...points.slice(0,8),points[7]],bed,options),/distinct/);
  const altered=points.map((p,i)=>i===6?{image:p.image,work:[p.work[0]+2,p.work[1]]}:p);
  assert.throws(()=>reg.calibrate(altered,bed,options),/misses by|reached/);
  assert.throws(()=>reg.calibrate(points,bed,{...options,tolerance:-1}),/error/);
  const cluster=[[.51,.5],[.49,.5],[.5,.51],[.5,.49],[.5,.5]].map(image=>({image,work:expected(image)}));
  assert.throws(()=>reg.calibrate([...points.slice(0,4),...cluster],bed,options),/Spread/);
});
test('distorted straight pixel traces acquire intermediate work coordinates with a preserved closing seam',()=>{
  const f=cameraFixture(-.4),cal=reg.calibrate(f.points,bed,f.options);
  const outline=[[25,25],[75,25],[75,68],[25,68],[25,25]];
  const path=reg.place([outline],[100,100],cal,bed)[0];
  assert.ok(path.length>outline.length);
  close(path[0],path.at(-1));
  assert.throws(()=>reg.place([[[0,0],[100,100]]],[100,100],cal,bed),/outside/);
});
