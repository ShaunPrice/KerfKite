const test=require('node:test');
const assert=require('node:assert/strict');
const layout=require('../../studio/static/text-tool.js');
const close=(a,b,tolerance=1e-9)=>assert.ok(Math.abs(a-b)<=tolerance,`${a} differs from ${b}`);

test('legacy defaults preserve the earlier symmetric baseline exactly',()=>{
 const paths=[[[3,-2],[13,5],[23,0],[3,-2]]],rise=8,width=20,minx=3;
 const expected=paths.map(p=>p.map(([x,y])=>{const t=(x-minx)/width,by=3*rise*t*(1-t),slope=3*rise*(1-2*t)/width,n=Math.hypot(1,slope);return[x-y*slope/n,by+y/n];}));
 assert.deepEqual(layout.warp(paths,{rise}),expected);
 assert.deepEqual(layout.warp(paths,{mode:'symmetric',rise}),expected);
});
test('independent control points change both baseline coordinates',()=>{
 const controls=layout.baseline(100,{mode:'custom',c1x:10,c1y:40,c2x:70,c2y:-20});
 assert.deepEqual(controls,[[0,0],[10,40],[70,-20],[100,0]]);
 const midpoint=layout.point(controls,.5);close(midpoint[0],42.5);close(midpoint[1],7.5);
 const alternative=layout.baseline(100,{mode:'custom',c1x:30,c1y:-10,c2x:90,c2y:60});
 assert.notDeepEqual(layout.point(alternative,.5),midpoint);
});
test('normal placement follows the custom curve while preserving outline closure',()=>{
 const paths=[[[10,0],[60,0],[110,0],[60,5],[10,0]]];
 const warped=layout.warp(paths,{mode:'custom',c1x:10,c1y:40,c2x:70,c2y:-20});
 close(warped[0][1][0],52.5);close(warped[0][1][1],7.5);
 close(Math.hypot(warped[0][3][0]-warped[0][1][0],warped[0][3][1]-warped[0][1][1]),5);
 assert.deepEqual(warped[0][0],warped[0].at(-1));
 assert.deepEqual(paths[0][1],[60,0]);
});
test('custom symmetric control points match legacy geometry within floating precision',()=>{
 const paths=[[[0,0],[20,8],[40,2],[60,-3],[80,10],[100,0]]];
 const old=layout.warp(paths,{rise:25}),custom=layout.warp(paths,{mode:'custom',c1x:100/3,c1y:25,c2x:200/3,c2y:25});
 old[0].forEach((p,i)=>p.forEach((v,k)=>close(v,custom[0][i][k])));
});
test('analytic curve bounds contain samples and find interior extrema',()=>{
 const p=layout.baseline(100,{mode:'custom',c1x:-50,c1y:100,c2x:150,c2y:-80});
 const b=layout.curveBounds(p);assert.ok(b[0]<0);assert.ok(b[2]>100);
 for(let i=0;i<=1000;i++){const[x,y]=layout.point(p,i/1000);assert.ok(x>=b[0]-1e-9&&x<=b[2]+1e-9&&y>=b[1]-1e-9&&y<=b[3]+1e-9);}
 const arch=layout.curveBounds(layout.baseline(100,{rise:100}));close(arch[3],75);
});
test('stationary endpoint and interior cusp are rejected before normals are computed',()=>{
 assert.throws(()=>layout.baseline(100,{mode:'custom',c1x:0,c1y:0,c2x:60,c2y:10}),/stationary tangent/);
 assert.throws(()=>layout.baseline(100,{mode:'custom',c1x:100,c1y:0,c2x:0,c2y:0}),/stationary tangent/);
});
test('finite values, option bounds and path budgets are enforced',()=>{
 for(const width of [0,-1,Infinity,NaN,2001])assert.throws(()=>layout.baseline(width),/finite/);
 for(const change of [{rise:NaN},{mode:'other'},{mode:'custom',c1x:-101},{mode:'custom',c2x:201},{mode:'custom',c1y:Infinity},{mode:'custom',c2y:2001}])assert.throws(()=>layout.baseline(100,change));
 assert.throws(()=>layout.point([[0,0],[1,1],[2,1],[3,0]],2),/parameter/);
 assert.throws(()=>layout.curveBounds([[0,0],[1,NaN],[2,1],[3,0]]),/finite/);
 assert.throws(()=>layout.warp([[[0,0],[0,1]]]),/zero width/);
 assert.throws(()=>layout.warp([[[0,0],[NaN,1]]]),/finite/);
 assert.throws(()=>layout.warp([Array.from({length:100001},(_,i)=>[i%2,0])]),/too many vertices/);
});
