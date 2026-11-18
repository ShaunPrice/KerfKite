import base64
from io import BytesIO
import unittest

from pypdf import PdfWriter
from pypdf.generic import (
    ArrayObject,
    DecodedStreamObject,
    DictionaryObject,
    NameObject,
    NumberObject,
    RectangleObject,
)

from studio.pdf_import import import_pdf
from validation.pdf_import_workflow import add_page, example_pdf


def source(writer, page=1):
    buffer = BytesIO()
    writer.write(buffer)
    return dict(content=base64.b64encode(buffer.getvalue()).decode('ascii'), page=page)


class PDFTests(unittest.TestCase):
    def test_multiple_pages_scale_cubic_and_discontinuous_strokes(self):
        payload = dict(content=base64.b64encode(example_pdf()).decode('ascii'))
        blank = import_pdf(payload)
        self.assertEqual(blank['pageCount'], 2)
        self.assertEqual(blank['objects'], [])
        self.assertIn('text objects omitted', ' '.join(blank['warnings']))
        result = import_pdf(dict(payload, page=2))
        self.assertEqual(result['layers'][0]['name'], 'PDF page 2')
        line, arch = (o['paths'][0] for o in result['objects'])
        self.assertAlmostEqual(line[1][0] - line[0][0], 25.4)
        self.assertGreater(len(arch), 8)
        self.assertAlmostEqual(max(p[1] for p in arch), 63 * 25.4 / 72, places=4)
        for value in [True, 0, 3, 1.5]:
            with self.assertRaises(ValueError):
                import_pdf(dict(payload, page=value))

    def test_rotate_crop_userunits_forms_and_compound_holes(self):
        writer = PdfWriter()
        page = add_page(writer, b'q 1 0 0 1 18 36 cm /Logo Do Q')
        page.cropbox = RectangleObject((18, 36, 126, 144))
        page.rotate(90)
        page[NameObject('/UserUnit')] = NumberObject(2)
        form = DecodedStreamObject()
        form.update(
            {
                NameObject('/Type'): NameObject('/XObject'),
                NameObject('/Subtype'): NameObject('/Form'),
                NameObject('/BBox'): RectangleObject((0, 0, 36, 36)),
                NameObject('/Matrix'): ArrayObject([NumberObject(i) for i in (2, 0, 0, 1, 0, 0)]),
            }
        )
        form.set_data(b'0 0 36 36 re 6 6 24 24 re f*')
        resources = DictionaryObject(
            {NameObject('/XObject'): DictionaryObject({NameObject('/Logo'): writer._add_object(form)})}
        )
        page[NameObject('/Resources')] = writer._add_object(resources)
        result = import_pdf(source(writer))
        self.assertEqual(len(result['objects']), 1)
        outer, hole = result['objects'][0]['paths']
        self.assertEqual(len(outer), 5)
        self.assertEqual(hole[0], hole[-1])
        for value, expected in zip(result['pageSize'], [76.2, 76.2]):
            self.assertAlmostEqual(value, expected)
        for actual, wanted in zip(outer, [(0, 76.2), (0, 25.4), (25.4, 25.4), (25.4, 76.2), (0, 76.2)]):
            for n, m in zip(actual, wanted):
                self.assertAlmostEqual(n, m)

    def test_clipping_dash_transparent_and_state_restore(self):
        writer = PdfWriter()
        page = add_page(
            writer,
            b'''
          q 10 10 25 25 re W n 12 12 m 28 12 l S 12 12 m 48 12 l S Q
          q [3 2] 0 d 10 10 m 30 30 l S Q
          q /Hide gs 20 20 m 25 25 l S Q
          40 40 m 60 60 l S
        ''',
        )
        page[NameObject('/Resources')] = DictionaryObject(
            {
                NameObject('/ExtGState'): DictionaryObject(
                    {NameObject('/Hide'): DictionaryObject({NameObject('/CA'): NumberObject(0)})}
                )
            }
        )
        result = import_pdf(source(writer))
        self.assertEqual(len(result['objects']), 2)
        notes = ' '.join(result['warnings'])
        for message in ('clipping boundary', 'dashed strokes omitted', 'non-visible paint omitted'):
            self.assertIn(message, notes)

    def test_nonzero_winding_and_overlapping_fills(self):
        writer = PdfWriter()
        add_page(writer, b'0 0 50 50 re 5 5 20 20 re f 80 10 30 30 re 95 25 30 30 re f')
        result = import_pdf(source(writer))
        self.assertEqual(len(result['objects']), 1)
        self.assertEqual(len(result['objects'][0]['paths']), 1)
        self.assertIn('overlapping or self-intersecting fill omitted', ' '.join(result['warnings']))

    def test_encrypted_and_malformed_documents(self):
        writer = PdfWriter()
        add_page(writer, b'0 0 m 5 5 l S')
        writer.encrypt('test-document')
        with self.assertRaisesRegex(ValueError, 'unencrypted'):
            import_pdf(source(writer))
        for content in (b'%PDF-1.5\nmissing xref', b'not a PDF'):
            with self.assertRaises(ValueError):
                import_pdf(dict(content=base64.b64encode(content).decode()))
