import unittest
from shapely.geometry import LineString,Point
from studio.operations import prepare,region
from studio.cam import generate

OUT=[[10,10],[30,10],[30,30],[10,30],[10,10]]
HOLE=[[18,18],[22,18],[22,22],[18,22],[18,18]]
class OperationTests(unittest.TestCase):
 def test_outside_offset_hole_shrinks_and_cut_edge_matches(self):
  p,_,_=prepare({'mode':'router','operation':'outside','tool_diameter':2},[OUT,HOLE])
  g=region(p);self.assertEqual(g.bounds,(9,9,31,31))
  self.assertFalse(g.contains(Point(20,20)));self.assertTrue(g.contains(Point(18.5,20)))
 def test_pocket_keeps_tool_disk_in_region_and_island(self):
  p,_,_=prepare({'mode':'router','operation':'pocket','tool_diameter':2,'stepover':.8},[OUT,HOLE]);g=region([OUT,HOLE])
  for path in p:self.assertLess(LineString(path).buffer(.999).difference(g).area,1e-8)
 def test_fill_excludes_holes(self):
  p,_,_=prepare({'mode':'laser','operation':'fill','interval':.5},[OUT,HOLE]);g=region([OUT,HOLE])
  for path in p:self.assertTrue(g.covers(LineString(path)))
 def test_knife_corner_swivel_and_no_energy(self):
  job=generate(dict(mode='knife',width=100,height=100,feed=100,power=1000,paths=[OUT],knife_offset=.5,knife_lead=1,depth=.1))
  self.assertFalse(any(l.startswith(('M3','M4')) for l in job['lines']));self.assertGreater(len(job['prepared_paths'][0]),10)
 def test_vcarve_depths_bounded(self):
  p,z,_=prepare({'mode':'router','operation':'vcarve','tool_diameter':6,'depth':2,'v_resolution':1},[OUT]);self.assertEqual(len(p),len(z));self.assertTrue(all(-2<=v<=0 for q in z for v in q))
 def test_compensated_bounds_rejected(self):
  with self.assertRaises(ValueError):generate(dict(mode='router',operation='outside',tool_diameter=4,width=30,height=30,feed=100,power=1000,paths=[OUT]))

class OperationRegressionTests(unittest.TestCase):
 def test_pocket_clears_accessible_centre(self):
  from shapely.ops import unary_union
  square=[[10,10],[31,10],[31,31],[10,31],[10,10]]
  p,_,_=prepare({'mode':'router','operation':'pocket','tool_diameter':2,'stepover':1},[square])
  swept=unary_union([LineString(path).buffer(1,quad_segs=32) for path in p])
  accessible=region([square]).buffer(-1).buffer(1,quad_segs=32)
  self.assertLess(accessible.difference(swept).area,.001)
  with self.assertRaises(ValueError):prepare({'mode':'router','operation':'pocket','tool_diameter':2,'stepover':2},[square])
 def test_ordered_job_cannot_silently_change_cutters(self):
  base=dict(mode='router',protocol='grbl',width=100,height=100,feed=100,power=1000)
  ops=[dict(paths=[OUT],operation='pocket',tool_diameter=2,stepover=.5),dict(paths=[OUT],operation='outside',tool_diameter=4)]
  with self.assertRaises(ValueError):generate({**base,'operations':ops})
 def test_raster_powers_keep_overscan_off(self):
  job=generate(dict(mode='laser',width=100,height=100,feed=1000,power=1000,paths=[[[1,1],[2,1],[3,1],[4,1]]],path_powers=[[0,0,750,0]]))
  self.assertIn('M4 S0',job['lines']);self.assertIn('G1 X3.000 Y1.000 F1000.0 S750',job['lines']);self.assertIn('G1 X4.000 Y1.000 F1000.0 S0',job['lines'])
