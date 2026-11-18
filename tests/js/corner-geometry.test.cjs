const assert = require('node:assert/strict');
const test = require('node:test');
const {intersection,nearest,joinPaths,channel} = require('../../studio/static/corner-geometry.js');

function join(a,xy,b,uv) {
  return joinPaths(a,nearest(a,xy),b,nearest(b,uv));
}
test('finite segment crossing accepts T and endpoint joints and rejects collinear, disjoint and degenerate segments', () => {
  assert.deepEqual(intersection([0,0],[10,10],[0,10],[10,0]),{point:[5,5],t:.5,u:.5});
  assert.deepEqual(intersection([0,0],[5,0],[5,4],[5,-4]).point,[5,0]);
  assert.deepEqual(intersection([0,0],[5,0],[5,0],[8,3]).point,[5,0]);
  assert.equal(intersection([0,0],[10,0],[3,0],[8,0]),null);
  assert.equal(intersection([0,0],[10,0],[11,1],[11,-1]),null);
  assert.equal(intersection([0,0],[0,0],[0,1],[0,-1]),null);
});
test('all four retained-arm choices give exactly one vertex at a perpendicular crossing', () => {
  const a = [[0,5],[10,5]], b = [[5,0],[5,10]];
  for (const endA of a) for (const endB of b) {
    assert.deepEqual(join(a,endA,b,endB).path,[endA,[5,5],endB]);
  }
  assert.throws(()=>join(a,[5,5],b,[5,0]),/farther/);
  assert.deepEqual(join([[0,5],[5,5]],[5,5],b,[5,9]).path,[[0,5],[5,5],[5,10]]);
});
test('dense sampled straight spans retain upstream polyline vertices and the interpolated Z/power value', () => {
  const a = [[-2,-3],...Array.from({length:21},(_,i)=>[i,0])];
  const b = [[15.5,-4],[15.5,-1],[15.5,3],[16,4]];
  const edit = join(a,[2,0],b,[15.5,2]);
  assert.deepEqual(edit.path,[...a.slice(0,17),[15.5,0],[15.5,3],[16,4]]);
  const depthA = a.map(([x])=>x/10);
  const depthB = [0,1.05,3.05,4];
  const z = channel(depthA,depthB,edit);
  assert.equal(z.length,edit.path.length);
  assert.equal(z.at(-3),1.55);
  assert.throws(()=>channel(depthA,depthB.map(z=>z+1),edit),/differs/);
});
test('a crossing exactly at sampled vertices is emitted once; a nearby bend never supplies a projected intersection', () => {
  const a = [[0,0],[1,1],[2,2],[3,3]], b = [[0,2],[1,1],[2,0]];
  assert.deepEqual(join(a,[0,0],b,[2,0]).path,[[0,0],[1,1],[2,0]]);
  assert.throws(()=>join([[0,0],[3,0],[3,4]],[1,0],[[2,2],[5,2]],[4,2]),/single crossing/);
  assert.throws(()=>join([[0,0],[3,0],[3,4],[0,0]],[1,0],b,[2,0]),/open/);
});
