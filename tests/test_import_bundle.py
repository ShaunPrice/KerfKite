import base64
import io
import stat
import unittest
import zipfile

from studio.import_bundle import inspect_bundle


def encoded(items):
    blob = io.BytesIO()
    with zipfile.ZipFile(blob, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
        for name, value in items:
            archive.writestr(name, value)
    return base64.b64encode(blob.getvalue()).decode()


class BundleTests(unittest.TestCase):
    def test_list_select_disambiguate_and_omit_other_files(self):
        b = encoded(
            [
                ('devices/one.lbdev', '{}'),
                ('materials/ref.clb', '<library/>'),
                ('artwork.lbart', b'123'),
                ('other/one.lbdev', '{"n":2}'),
            ]
        )
        summary = inspect_bundle({'content': b})
        self.assertEqual(
            summary['files'],
            [{'index': 0, 'name': 'one.lbdev'}, {'index': 1, 'name': 'ref.clb'}, {'index': 3, 'name': 'one.lbdev'}],
        )
        self.assertEqual(summary['skipped'], 1)
        self.assertEqual(inspect_bundle({'content': b, 'index': 3})['text'], '{"n":2}')
        for i in (True, -1, 2, 300, '0', None):
            with self.assertRaises(ValueError):
                inspect_bundle({'content': b, 'index': i})

    def test_member_types_and_text_encoding(self):
        linked = zipfile.ZipInfo('linked.clb')
        linked.external_attr = (stat.S_IFLNK | 0o777) << 16
        b = encoded(
            [
                ('../wrong.clb', 'x'),
                (linked, 'x'),
                ('a.lbset', 'α'.encode('utf-16')),
                ('b.lbdev', b'\xff\xab'),
                ('c.lbrn2', b'\x00bin'),
            ]
        )
        self.assertEqual(len(inspect_bundle({'content': b})['files']), 3)
        self.assertEqual(inspect_bundle({'content': b, 'index': 2})['text'], 'α')
        for i in (1, 3, 4):
            with self.assertRaises(ValueError):
                inspect_bundle({'content': b, 'index': i})

    def test_deterministic_caps_and_invalid_content(self):
        for b in (
            'not base64',
            'AQID',
            'A' * 5_333_340,
            encoded([(f'{i}.lbdev', 'a') for i in range(257)]),
            encoded([('large.clb', b'q' * 4_000_001)]),
        ):
            with self.assertRaises(ValueError):
                inspect_bundle({'content': b})
