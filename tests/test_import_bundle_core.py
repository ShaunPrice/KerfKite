"""The native core's LightBurn bundle reader against the Python reference (zipfile and zlib), to the bit."""

import base64
import io
import json
import random
import struct
import unittest
import warnings
import zipfile

from studio.import_bundle import import_json, inspect_bundle
from studio.server import create_app
from validation.native_core_parity import bundle_archive, bundle_import_cases

try:
    from studio import _relief_native as native
except ImportError:
    native = None

AVAILABLE = native is not None and hasattr(native, 'core_cam')


def outcome(given, backend):
    """The answer as text that tells every bit, or the error; None when the core declines."""
    try:
        with warnings.catch_warnings():
            warnings.simplefilter('ignore')
            return json.dumps(inspect_bundle(given, backend=backend), sort_keys=True)
    except ValueError as error:
        return 'ValueError: ' + str(error)
    except NotImplementedError:
        return None


def encoded(blob):
    return base64.b64encode(blob).decode('ascii')


def written(items):
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
        for name, value in items:
            archive.writestr(name, value)
    return buffer.getvalue()


@unittest.skipUnless(AVAILABLE, 'the compiled extension is not installed')
class BundleCoreTests(unittest.TestCase):
    def same(self, given):
        core = outcome(given, 'native')
        self.assertIsNotNone(core, 'the core declined')
        self.assertEqual(core, outcome(given, 'python'))
        return core

    def test_listing_and_members_match(self):
        blob = written([
            ('devices/one.lbdev', '{"name": "Laser"}'),
            ('materials/ref.clb', '<Library/>'),
            ('Prefs/réglage.LBPREFS', 'réglage = 1'.encode('utf-16')),
            ('art.lbart', b'123'),
            ('folder/', ''),
            ('sig.lbset', b'\xef\xbb\xbf{"set": true}'),
            ('be.lbrn2', b'\xfe\xff' + 'big end \U0001f600'.encode('utf-16-be')),
        ])  # fmt: skip
        listing = json.loads(self.same({'content': encoded(blob)}))
        self.assertEqual([f['index'] for f in listing['files']], [0, 1, 2, 5, 6])
        self.assertEqual(listing['skipped'], 1)
        for index in range(7):
            self.same({'content': encoded(blob), 'index': index})
        self.assertIn('\U0001f600', json.loads(self.same({'content': encoded(blob), 'index': 6}))['text'])

    def test_refusals_match(self):
        sound = written([('one.lbdev', '{}')])
        for given in (
            [],
            {'content': 5},
            {'content': 'not base64!'},
            {'content': ''},
            {'content': encoded(b'not a zip archive')},
            {'content': encoded(sound[:-5])},
            {'content': encoded(sound), 'index': True},
            {'content': encoded(sound), 'index': 1.0},
        ):
            self.assertTrue(self.same(given).startswith('ValueError: '), given)
        # An index beyond JSON's integers does not reach the core unchanged; Python answers it.
        given = {'content': encoded(sound), 'index': 10**30}
        self.assertIsNone(outcome(given, 'native'))
        self.assertEqual(outcome(given, 'auto'), 'ValueError: Select a supported member of this bundle')

    def test_damage_is_read_as_zipfile_reads_it(self):
        rng = random.Random(3)
        member = {'name': b'one.lbdev', 'data': b'{"speed": 300}' * 40}
        cases = [
            dict(member, crc=0),
            dict(member, size=member['data'].__len__() + 1),
            dict(member, local_name=b'two.lbdev'),
            dict(member, csize=3),
            dict(member, method=0, csize=5),
            dict(member, extra=b'\xfe\xca\x09\x00abc'),
            dict(member, data=b'bin\x00ary'),
            dict(member, data=b'\xff\xfe\x00\xd8A\x00'),
            dict(member, name='été.clb'.encode('cp437')),
            dict(member, name='été.clb'.encode('utf-8'), flags=0x800),
            dict(member, name=b'\xff.clb', flags=0x800),
            dict(member, external=0o120777 << 16),
            dict(member, flags=1),
        ]
        for m in cases:
            blob = bundle_archive([m], rng, prefix=b'stub', comment=b'PK\x05\x06x')
            for given in ({'content': encoded(blob)}, {'content': encoded(blob), 'index': 0}):
                self.same(given)

    def test_what_versions_read_differently_is_declined(self):
        rng = random.Random(4)
        member = {'name': b'one.lbdev', 'data': b'{}'}
        for m in (
            dict(member, version=64),
            dict(member, extra=b'\x01\x00\x08\x00' + bytes(8)),
            dict(member, extra=b'\x75\x70\x05\x00\x01abcd'),
        ):
            given = {'content': encoded(bundle_archive([m], rng))}
            self.assertIsNone(outcome(given, 'native'), m)
        blob = bundle_archive([member], rng)
        zip64 = blob[:-22] + b'PK\x06\x07' + bytes(16) + blob[-22:]
        self.assertIsNone(outcome({'content': encoded(zip64)}, 'native'))
        # A member that inflates past what is read declines, and Python answers it.
        bomb = bundle_archive([{'name': b'big.lbdev', 'data': bytes(4_100_000), 'size': 100}], rng)
        given = {'content': encoded(bomb), 'index': 0}
        self.assertIsNone(outcome(given, 'native'))
        self.assertEqual(outcome(given, 'auto'), outcome(given, 'python'))

    def test_generated_requests_match(self):
        answered = 0
        for given in bundle_import_cases(400, 9):
            core = outcome(given, 'native')
            if core is not None:
                answered += 1
                self.assertEqual(core, outcome(given, 'python'), str(given)[:200])
        self.assertGreater(answered, 300)

    def test_route_sends_the_core_text(self):
        app = create_app()
        app.testing = True
        client = app.test_client()
        token = client.get('/api/session').get_json()['token']
        blob = written([('one.lbdev', '{"a": 1}')])
        given = {'content': encoded(blob), 'index': 0}
        reply = client.post('/api/import-bundle', json=given, headers={'X-Studio-Token': token})
        self.assertEqual(reply.status_code, 200)
        self.assertEqual(reply.get_data(), import_json(given))
        self.assertEqual(reply.get_json(), {'name': 'one.lbdev', 'text': '{"a": 1}'})


class BundleFallbackTests(unittest.TestCase):
    def test_python_answers_alone(self):
        blob = written([('one.lbdev', '{}')])
        self.assertEqual(
            inspect_bundle({'content': encoded(blob)}, backend='python')['files'], [{'index': 0, 'name': 'one.lbdev'}]
        )
        with self.assertRaises(ValueError):
            inspect_bundle({'content': encoded(blob)}, backend='other')

    def test_archive_writer_is_a_zip(self):
        blob = bundle_archive([{'name': b'one.lbdev', 'data': b'{}'}], random.Random(1))
        with zipfile.ZipFile(io.BytesIO(blob)) as archive:
            self.assertEqual(archive.read('one.lbdev'), b'{}')
        self.assertEqual(struct.unpack('<4s', blob[:4])[0], b'PK\x03\x04')


if __name__ == '__main__':
    unittest.main()
