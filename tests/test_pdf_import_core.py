"""The native core's PDF reader against the Python reference (pypdf and Shapely), to the bit."""

import base64
import json
import unittest
from io import BytesIO
from unittest.mock import patch

from pypdf import PdfWriter

from studio import pdf_import
from studio.pdf_import import import_json, import_pdf
from studio.server import create_app
from validation.native_core_parity import pdf_document
from validation.pdf_import_workflow import add_page, example_pdf

try:
    from studio import _relief_native as native
except ImportError:
    native = None

AVAILABLE = native is not None and hasattr(native, 'core_cam')


def payload(data, page=1):
    return {'content': base64.b64encode(data).decode('ascii'), 'page': page}


def outcome(given, backend):
    """The answer as text that tells every bit, or the error; None when the core declines."""
    try:
        return json.dumps(import_pdf(given, backend=backend), sort_keys=True)
    except ValueError as error:
        return 'ValueError: ' + str(error)
    except NotImplementedError:
        return None


class Rng:
    """random.Random's choice and random, fixed: the first choice and no randomness."""

    def choice(self, options):
        return options[0]

    def random(self):
        return 0.9


@unittest.skipUnless(AVAILABLE, 'the compiled extension is not installed')
class NativePDFTests(unittest.TestCase):
    def same(self, given):
        mine = outcome(given, 'native')
        self.assertIsNotNone(mine, 'the core declined')
        self.assertEqual(mine, outcome(given, 'python'))

    def test_pages_are_read_as_the_reference_reads_them(self):
        for page in (1, 2):
            self.same(payload(example_pdf(), page))
        contents = [
            b'q 2 0 0 2 18 18 cm 0 0 m 36 0 l S Q 18 36 m 30 72 52 72 68 36 c S',
            b'0 0 50 50 re 5 5 20 20 re f* 80 10 30 30 re 95 25 30 30 re f',
            b'10 10 m 60 10 l 60 60 l h 20 20 m 40 20 l 40 40 l h f 0 0 m 5 5 l 9 9 10 2 v 3 3 4 4 y s',
            b'q 20 20 80 80 re W n 0 0 m 100 100 l S 30 30 10 10 re f Q /GS0 gs 5 5 m 9 9 l S',
            b'BT /F1 12 Tf (Label) Tj ET /OC /L1 BDC 0 0 m 1 1 l S EMC [2 1] 0 d 0 0 m 50 0 l S',
        ]
        for content in contents:
            with self.subTest(content=content):
                writer = PdfWriter()
                add_page(writer, content)
                buffer = BytesIO()
                writer.write(buffer)
                self.same(payload(buffer.getvalue()))

    def test_cross_reference_streams_object_streams_and_filters(self):
        page = {'content': b'0 0 40 40 re 10 10 20 20 re f* 50 50 m 90 60 l 70 90 l h S', 'states': {}, 'forms': {}}
        for table in (True, False):
            for packed in (False, True):
                with self.subTest(table=table, packed=packed):
                    self.same(payload(pdf_document([dict(page)], Rng(), objects_in_streams=packed, table=table)))

    def test_refusals_carry_the_reference_messages(self):
        writer = PdfWriter()
        add_page(writer, b'0 0 m 5 5 l S')
        buffer = BytesIO()
        writer.write(buffer)
        good = buffer.getvalue()
        for given in (
            {'content': 'not base64!'},
            payload(b'not a PDF'),
            payload(b'%PDF-1.5\nmissing xref'),
            payload(good, 0),
            payload(good, 2),
            dict(payload(good), page=True),
            dict(payload(good), page=1.0),
            [],
            {'page': 1},
        ):
            with self.subTest(given=str(given)[:60]):
                self.same(given)
        for content, message in (
            (b'q 0 0 m 5 5 l S', 'unclosed graphics state'),
            (b'Q', 'Unmatched PDF graphics-state restore'),
            (b'5 5 l S', 'missing its starting point'),
            (b'0 0 m 1 2 3 l S', 'wrong number of coordinates'),
            (b'/Missing gs', 'missing graphics state'),
            (b'99999999999 0 m 1 1 l S', 'supported numeric range'),
        ):
            with self.subTest(content=content):
                writer = PdfWriter()
                add_page(writer, content)
                buffer = BytesIO()
                writer.write(buffer)
                given = payload(buffer.getvalue())
                self.same(given)
                self.assertIn(message, outcome(given, 'native'))

    def test_encrypted_documents_are_refused_or_left_to_the_reference(self):
        for algorithm, declined in (('RC4-128', False), ('AES-256', True)):
            writer = PdfWriter()
            add_page(writer, b'0 0 m 5 5 l S')
            writer.encrypt('test-document', algorithm=algorithm)
            buffer = BytesIO()
            writer.write(buffer)
            given = payload(buffer.getvalue())
            if declined:
                self.assertIsNone(import_json(given))
            else:
                self.assertEqual(outcome(given, 'native'), 'ValueError: Export an unencrypted PDF for vector review')
            self.assertEqual(outcome(given, 'auto'), outcome(given, 'python'))

    def test_the_route_sends_the_core_text(self):
        client = create_app().test_client()
        headers = {'X-Studio-Token': client.get('/api/session').json['token']}
        given = payload(example_pdf(), 2)
        with patch.object(pdf_import, 'import_pdf', wraps=pdf_import.import_pdf) as reference:
            response = client.post('/api/pdf-import', json=given, headers=headers)
        reference.assert_not_called()
        self.assertEqual(response.status_code, 200)
        self.assertEqual(response.get_data(), import_json(given))
        self.assertEqual(response.get_json(), json.loads(json.dumps(import_pdf(given, backend='python'))))
        failing = client.post('/api/pdf-import', json={'content': 'not base64!'}, headers=headers)
        self.assertEqual(failing.status_code, 400)
        self.assertEqual(failing.get_json()['error'], 'Invalid PDF file encoding')

    def test_inflate_reads_every_kind_of_block(self):
        # Stored, fixed and dynamic blocks: FlateDecode content compressed at levels 0, 1 and 9.
        content = b' '.join(b'%d %d m %d %d l S' % (i, i % 7, i + 3, i % 11) for i in range(400))
        for level in (0, 1, 9):
            with self.subTest(level=level):
                writer = PdfWriter()
                page = add_page(writer, content)
                page.compress_content_streams(level=level)
                buffer = BytesIO()
                writer.write(buffer)
                self.assertIn(b'/FlateDecode', buffer.getvalue())
                self.same(payload(buffer.getvalue()))

    def test_backend_names_are_checked(self):
        with self.assertRaisesRegex(ValueError, 'auto, python or native'):
            import_pdf({'content': ''}, backend='fast')


if __name__ == '__main__':
    unittest.main()
