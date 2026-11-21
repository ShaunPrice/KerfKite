"""Version 1 little-endian stock-preview transfer used by the desktop/web editor."""

from struct import Struct
from .cam_moves import PackedSegments

MIME = 'application/vnd.kerfkite.stock-v1'
_HEADER = Struct('<8sIII7d')
_MAGIC = b'KKSTOCK1'
_SHAPES = ('flat', 'ball', 'v', 'drill')
_ROW_SIZE = 7 * 8
MAX_ROWS = 200000
MAX_BINARY_BYTES = _HEADER.size + MAX_ROWS * _ROW_SIZE


def from_stream(stream, content_length):
    header = stream.read(_HEADER.size)
    if len(header) != _HEADER.size:
        raise ValueError('Truncated stock preview header')
    magic, count, resolution, shape, x, y, width, height, diameter, angle, thickness = _HEADER.unpack(header)
    if magic != _MAGIC or count > MAX_ROWS or shape >= len(_SHAPES):
        raise ValueError('Invalid stock preview header')
    size = count * _ROW_SIZE
    if content_length is not None and content_length != _HEADER.size + size:
        raise ValueError('Stock preview length differs from segment count')
    rows = stream.read(size)
    if len(rows) != size or stream.read(1):
        raise ValueError('Stock preview length differs from segment count')
    return dict(
        segments=PackedSegments.from_little_endian(rows),
        resolution=resolution,
        x=x,
        y=y,
        width=width,
        height=height,
        tool_diameter=diameter,
        v_angle=angle,
        tool_shape=_SHAPES[shape],
        stock_thickness=thickness,
    )
