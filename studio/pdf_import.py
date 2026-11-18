"""Extract authored PDF vector paths into a review draft, in millimetres."""

import base64
from collections import Counter
from dataclasses import dataclass, replace
from io import BytesIO
import math

from pypdf import PdfReader, apply_configuration
from pypdf.errors import PyPdfError
from pypdf.generic import ArrayObject, ContentStream, StreamObject
from shapely.geometry import MultiPoint, Polygon
from shapely.strtree import STRtree

from .program_import import Drawing, MAX_PATHS, MAX_POINTS
from .sketch_curves import sample_cubic


IDENTITY = (1.0, 0.0, 0.0, 1.0, 0.0, 0.0)
MAX_BYTES = 4_000_000


def numbers(args, count):
    if len(args) != count:
        raise ValueError('PDF path command has the wrong number of coordinates')
    values = tuple(float(v) for v in args)
    if not all(math.isfinite(v) and abs(v) <= 1e9 for v in values):
        raise ValueError('PDF coordinates exceed the supported numeric range')
    return values


def compose(outer, inner):
    a, b, c, d, e, f = outer
    g, h, i, j, k, l = inner
    return numbers(
        (a * g + c * h, b * g + d * h, a * i + c * j, b * i + d * j, a * k + c * l + e, b * k + d * l + f), 6
    )


def point(matrix, x, y):
    a, b, c, d, e, f = matrix
    return numbers((a * x + c * y + e, b * x + d * y + f), 2)


def rectangle(matrix, box):
    x, y, X, Y = numbers(box, 4)
    return Polygon([point(matrix, a, b) for a, b in ((x, y), (X, y), (X, Y), (x, Y))])


def content_stream(raw, reader):
    if raw is None:
        return None
    raw = raw.get_object()
    streams = raw if isinstance(raw, ArrayObject) else [raw]
    if len(streams) > 256:
        raise ValueError('Simplify the PDF page to at most 256 content streams')
    for item in streams:
        block = item.get_object()
        if not isinstance(block, StreamObject) or '/F' in block:
            raise ValueError('PDF uses an unsupported external content stream')
        filters = block['/Filter'] if '/Filter' in block else []
        if not isinstance(filters, list):
            filters = [filters]
        if any(
            name
            not in (
                '/FlateDecode',
                '/Fl',
                '/ASCII85Decode',
                '/A85',
                '/ASCIIHexDecode',
                '/AHx',
                '/LZWDecode',
                '/LZW',
                '/RunLengthDecode',
                '/RL',
            )
            for name in filters
        ):
            raise ValueError('Re-export this page using standard vector content compression')
    return ContentStream(raw, reader)


def fill_boundaries(contours, even_odd):
    """Keep visible boundaries of disjoint or nested simple rings under the PDF fill rule."""
    rings = [Polygon(p) for p in contours if len(set(p)) >= 3]
    if any(not ring.is_valid for ring in rings):
        return None
    tree = STRtree(rings)
    boundaries = []
    comparisons = 0
    for i, ring in enumerate(rings):
        winding = 0
        for idx in tree.query(ring):
            comparisons += 1
            if comparisons > 60_000:
                raise ValueError('Simplify the overlapping regions of the PDF fill')
            if i == idx:
                continue
            other = rings[idx]
            if other.boundary.intersects(ring.boundary):
                return None
            if other.contains(ring):
                winding += 1 if other.exterior.is_ccw else -1
        if even_odd or (winding == 0) != (winding + (1 if ring.exterior.is_ccw else -1) == 0):
            boundaries.append(list(ring.exterior.coords))
    return boundaries


@dataclass
class State:
    matrix: tuple
    clip: object
    dashed: bool = False
    stroke_alpha: float = 1.0
    fill_alpha: float = 1.0
    soft_mask: bool = False
    text_mode: int = 0


class Extraction:
    def __init__(self, page_number):
        self.layer = f'PDF page {page_number}'
        self.drawing = Drawing()
        self.skips = Counter()
        self.ops = self.constructed = self.forms = self.contours = 0

    def process(self, stream, resources, state, depth=0):
        if stream is None:
            return
        if depth > 12:
            raise ValueError('PDF forms nest more than 12 levels')
        paths, hull, stack, marks = [], [], [], []
        current = None
        pending_clip = False
        skip_text = False

        def add(points):
            if current is None:
                raise ValueError('PDF line or curve is missing its starting point')
            self.constructed += len(points)
            if self.constructed > MAX_POINTS:
                raise ValueError('Simplify the PDF to 100000 constructed vertices')
            current.extend(points)

        def close():
            if current and current[-1] != current[0]:
                add([current[0]])

        def paint(fill, stroke, even_odd):
            if marks and marks[-1]:
                self.skips['optional-content artwork omitted'] += 1
                return
            if stroke and state.dashed:
                self.skips['dashed strokes omitted; expand the dashes into separate paths'] += 1
                stroke = False
            fill = fill and state.fill_alpha > 0
            stroke = stroke and state.stroke_alpha > 0
            if state.soft_mask or not (fill or stroke):
                self.skips['masked or non-visible paint omitted'] += 1
                return
            if state.clip is None or not state.clip.covers(MultiPoint(hull)):
                self.skips[
                    'paint omitted because its full path could not be verified inside the clipping boundary'
                ] += 1
                return
            contours = []
            for path in paths:
                if len(path) < 2 or all(p == path[0] for p in path):
                    continue
                if fill and path[-1] != path[0]:
                    path = [*path, path[0]]
                contours.append(path)
            if fill and not stroke:
                contours = fill_boundaries(contours, even_odd)
                if contours is None:
                    self.skips[
                        'overlapping or self-intersecting fill omitted; unite the filled regions in the source'
                    ] += 1
                    return
            if not contours:
                return
            self.contours += len(contours)
            if self.contours > MAX_PATHS:
                raise ValueError('Simplify the PDF to 3000 vector paths')
            # A compound fill must travel as one object so nested boundaries remain together.
            draft = self.drawing
            group = None
            for contour in contours:
                draft.path(self.layer + ' artwork', self.layer, contour)
                if group is None:
                    group = draft.objects[-1]
                else:
                    group['paths'].extend(draft.objects.pop()['paths'])

        for args, op in stream.operations:
            self.ops += 1
            if self.ops > 100_000:
                raise ValueError('Simplify the PDF to 100000 graphics operations')
            if op == b'q':
                if len(stack) >= 64:
                    raise ValueError('PDF graphics-state nesting exceeds 64')
                stack.append(replace(state))
            elif op == b'Q':
                if not stack:
                    raise ValueError('Unmatched PDF graphics-state restore')
                state = stack.pop()
            elif op == b'cm':
                state.matrix = compose(state.matrix, numbers(args, 6))
            elif op == b'm':
                current = []
                paths.append(current)
                p = point(state.matrix, *numbers(args, 2))
                add([p])
                hull.append(p)
            elif op == b'l':
                p = point(state.matrix, *numbers(args, 2))
                add([p])
                hull.append(p)
            elif op in (b'c', b'v', b'y'):
                if not current:
                    raise ValueError('PDF curve is missing its starting point')
                values = numbers(args, 6 if op == b'c' else 4)
                poles = [point(state.matrix, *values[i : i + 2]) for i in range(0, len(values), 2)]
                if op == b'v':
                    poles.insert(0, current[-1])
                if op == b'y':
                    poles.append(poles[-1])
                if all(p == current[-1] for p in poles):
                    continue
                hull.extend(poles)
                add(sample_cubic(current[-1], poles[:2], poles[2]))
            elif op == b'h':
                close()
            elif op == b're':
                x, y, w, h = numbers(args, 4)
                current = []
                paths.append(current)
                corners = [
                    point(state.matrix, a, b) for a, b in ((x, y), (x + w, y), (x + w, y + h), (x, y + h))
                ]
                add([*corners, corners[0]])
                hull.extend(corners)
            elif op in (b'W', b'W*'):
                pending_clip = True
            elif op in (b'S', b's', b'f', b'F', b'f*', b'B', b'B*', b'b', b'b*', b'n'):
                if op in (b's', b'b', b'b*'):
                    close()
                if op != b'n' and hull:
                    paint(op not in (b'S', b's'), op not in (b'f', b'F', b'f*'), op.endswith(b'*'))
                if pending_clip and state.clip is not None:
                    # Convex straight-sided clipping keeps a definitive inside test.
                    # Curves are sampled, so use them only for artwork, never as a clip.
                    if len(paths) == 1 and 3 <= len(paths[0]) <= 5 and set(paths[0]) == set(hull):
                        poly = Polygon(paths[0])
                        if poly.is_valid and poly.area and poly.equals(poly.convex_hull):
                            state.clip = state.clip.intersection(poly)
                        else:
                            state.clip = None
                    else:
                        state.clip = None
                current = None
                paths = []
                hull = []
                pending_clip = False
            elif op == b'd':
                state.dashed = bool(args[0])
            elif op == b'gs':
                ext = resources['/ExtGState'].get(args[0]) if '/ExtGState' in resources else None
                if ext is None:
                    raise ValueError('PDF refers to a missing graphics state')
                ext = ext.get_object()
                if '/CA' in ext:
                    state.stroke_alpha = numbers([ext['/CA']], 1)[0]
                if '/ca' in ext:
                    state.fill_alpha = numbers([ext['/ca']], 1)[0]
                if '/SMask' in ext:
                    state.soft_mask = ext['/SMask'] != '/None'
                if '/D' in ext:
                    state.dashed = bool(ext['/D'][0])
            elif op in (b'BMC', b'BDC'):
                marks.append(bool(marks and marks[-1]) or args[0] == '/OC')
            elif op == b'EMC':
                if not marks:
                    raise ValueError('Unmatched PDF marked-content end')
                marks.pop()
            elif op == b'Tr':
                state.text_mode = int(numbers(args, 1)[0])
            elif op in (b'Tj', b'TJ', b"'", b'"'):
                self.skips['text objects omitted; convert text to paths in the source'] += 1
                skip_text = skip_text or state.text_mode >= 4
            elif op == b'ET' and skip_text:
                state.clip = None
                skip_text = False
            elif op in (b'INLINE IMAGE', b'sh'):
                self.skips['image or shading omitted; use a raster import for this artwork'] += 1
            elif op == b'Do':
                ref = resources['/XObject'].get(args[0]) if '/XObject' in resources else None
                if ref is None:
                    raise ValueError('PDF refers to a missing image or form')
                form = ref.get_object()
                if form.get('/Subtype') != '/Form':
                    self.skips['image omitted; use a raster import for this artwork'] += 1
                    continue
                if (
                    (marks and marks[-1])
                    or '/OC' in form
                    or '/Group' in form
                    or state.soft_mask
                    or state.clip is None
                ):
                    self.skips['optional, masked, clipped or transparency-group form omitted'] += 1
                    continue
                self.forms += 1
                if self.forms > 500:
                    raise ValueError('PDF has more than 500 form invocations')
                matrix = compose(state.matrix, numbers(form.get('/Matrix', IDENTITY), 6))
                form_state = replace(
                    state, matrix=matrix, clip=state.clip.intersection(rectangle(matrix, form['/BBox']))
                )
                self.process(
                    content_stream(form, stream.pdf),
                    form['/Resources'] if '/Resources' in form else resources,
                    form_state,
                    depth + 1,
                )
        if stack or marks:
            raise ValueError('PDF has an unclosed graphics state or marked-content block')


def import_pdf(payload):
    if not isinstance(payload, dict) or not isinstance(payload.get('content'), str):
        raise ValueError('Choose a PDF document')
    encoded = payload['content']
    if len(encoded) > 4 * ((MAX_BYTES + 2) // 3):
        raise ValueError('Choose a PDF no larger than 4 MB')
    try:
        data = base64.b64decode(encoded, validate=True)
    except (ValueError, TypeError) as exc:
        raise ValueError('Invalid PDF file encoding') from exc
    if len(data) > MAX_BYTES or not data.startswith(b'%PDF-'):
        raise ValueError('Choose a PDF document no larger than 4 MB')
    number = payload.get('page', 1)
    if type(number) is not int or number < 1:
        raise ValueError('Select a positive PDF page number')
    with apply_configuration(
        maximum_declared_stream_length=8_000_000,
        zlib_maximum_output_length=8_000_000,
        lzw_maximum_output_length=8_000_000,
        run_length_maximum_output_length=8_000_000,
        brotli_maximum_output_length=8_000_000,
        array_based_stream_maximum_output_length=8_000_000,
        stream_decoding_work_maximum_length=24_000_000,
        stream_parsing_work_maximum_count=200_000,
        page_tree_maximum_page_entries=500,
        page_tree_maximum_entries=1000,
    ):
        try:
            reader = PdfReader(BytesIO(data), strict=True)
            if reader.is_encrypted:
                raise ValueError('Export an unencrypted PDF for vector review')
            count = len(reader.pages)
            if count > 500:
                raise ValueError('Split the PDF into documents of at most 500 pages')
            if not 1 <= number <= count:
                raise ValueError(f'Choose a page from 1 to {count}')
            page = reader.pages[number - 1]
            scale = float(page.user_unit) * 25.4 / 72
            x, y, X, Y = numbers(page.cropbox, 4)
            mx, my, mX, mY = numbers(page.mediabox, 4)
            x, y, X, Y = max(x, mx), max(y, my), min(X, mX), min(Y, mY)
            if not 0 < scale <= 1e5 or not (x < X and y < Y):
                raise ValueError('Invalid PDF page scale or crop box')
            rotation = page.rotation % 360
            width, height = (X - x) * scale, (Y - y) * scale
            rotation_matrix = {
                0: IDENTITY,
                90: (0, -1, 1, 0, 0, width),
                180: (-1, 0, 0, -1, width, height),
                270: (0, 1, -1, 0, height, 0),
            }
            if rotation not in rotation_matrix:
                raise ValueError('PDF page rotation must be a multiple of 90 degrees')
            matrix = compose(rotation_matrix[rotation], (scale, 0, 0, scale, -x * scale, -y * scale))
            clip = rectangle(matrix, (x, y, X, Y))
            extraction = Extraction(number)
            extraction.process(
                content_stream(page.get('/Contents'), reader),
                page['/Resources'] if '/Resources' in page else {},
                State(matrix, clip),
            )
            if page.get('/Annots'):
                extraction.skips['annotations omitted'] += len(page['/Annots'])
        except (
            PyPdfError,
            KeyError,
            IndexError,
            AttributeError,
            TypeError,
            ArithmeticError,
            RecursionError,
        ) as exc:
            raise ValueError(
                'Cannot read this PDF page. Export a simplified, unencrypted vector PDF.'
            ) from exc
    drawing = extraction.drawing
    drawing.warnings.append(
        'Paths keep page scale. Strokes import as centrelines and fills as boundaries. Check stacked artwork, holes and orientation before choosing a cutting operation.'
    )
    drawing.warnings.extend(f'{amount} × {reason}.' for reason, amount in extraction.skips.items())
    if not drawing.objects:
        drawing.warnings.append(
            'No supported vector paths on this page. Select another page or convert source artwork to curves.'
        )
    result = (
        drawing.finish()
        if drawing.objects
        else dict(objects=[], layers=[], presets=[], devices=[], warnings=drawing.warnings, mode=None)
    )
    result.update(
        page=number, pageCount=count, pageSize=[height, width] if rotation in (90, 270) else [width, height]
    )
    return result
