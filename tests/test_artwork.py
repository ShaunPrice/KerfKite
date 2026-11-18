import json
import tempfile
import unittest
from pathlib import Path
from studio.artwork import ArtworkStore


class ArtworkTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup)
        self.store=ArtworkStore(self.tmp.name)
    def test_search_and_template_scaling(self):
        result=self.store.list_templates('flower')
        self.assertEqual(len(result),30)  # Default search page remains bounded.
        self.assertEqual(len(self.store.builtins),1000)
        self.assertEqual(len(self.store.list_templates(category='Flowers',limit=100)),56)
        self.assertEqual(len(self.store.list_templates(category='Nautical',limit=100)),8)
        self.assertTrue(all('flower' in (row['name']+' '+row['category']).casefold() for row in result))
        t=self.store.template(result[0]['id'],10,20,30,40)
        for path in t['paths']:
            for x,y in path:self.assertTrue(10<=x<=40 and 20<=y<=60)
    def test_portable_project_and_escaped_svg(self):
        paths=[[[1,1],[9,1],[9,9],[1,1]],[[4,4],[5,5]]]
        result=self.store.create_artwork('<test>',paths,100,100)
        d=json.loads(Path(result['project_path']).read_text())
        self.assertEqual(d['objects'][0]['paths'],paths)
        self.assertEqual(d['format'],'pi-cnc-studio/1')
        svg=Path(result['svg_path']).read_text()
        self.assertIn('&lt;test&gt;',svg);self.assertEqual(svg.count('<path '),2)
        self.assertEqual(Path(result['svg_path']).parent,Path(self.tmp.name).resolve())
    def test_template_bundle_normalizes(self):
        result=self.store.save_template('Leaf','Custom',[[[10,20],[20,40],[10,20]]])
        d=json.loads(Path(result['library_path']).read_text())
        self.assertEqual(d['format'],'pi-cnc-templates/1')
        self.assertEqual(d['custom'][0]['paths'],[[[0,0],[100,100],[0,0]]])
    def test_invalid_geometry_creates_no_artifacts(self):
        for paths in ([[[1,1],[float('nan'),2]]],[[[-1,0],[1,1]]],[[[0,0]]]):
            with self.assertRaises(ValueError):self.store.create_artwork('bad',paths)
        self.assertEqual(list(Path(self.tmp.name).iterdir()),[])
