"""Packed in-process CAM moves. Convert to lists only at a public JSON boundary."""

import math
import sys
from array import array
from dataclasses import dataclass
from struct import Struct


# Fields match the public preview row: start/end XY, end Z, laser power, start Z.
# Missing power (router/knife) is represented by NaN only in the packed buffer.
_ROW = Struct('=7d')


class PackedSegments:
    def __init__(self):
        self._data = bytearray()

    @classmethod
    def from_little_endian(cls, data):
        if not isinstance(data, bytes) or len(data) % _ROW.size:
            raise ValueError('Invalid packed segment buffer')
        segments = cls()
        if sys.byteorder != 'little':
            values = array('d')
            values.frombytes(data)
            values.byteswap()
            data = values.tobytes()
        segments._data = data
        return segments

    def _writable(self):
        if isinstance(self._data, bytes):
            self._data = bytearray(self._data)
        return self._data

    def append(self, row):
        x, y, nx, ny, z, power, start = row
        self._writable().extend(_ROW.pack(x, y, nx, ny, z, math.nan if power is None else power, start))

    def __len__(self):
        return len(self._data) // _ROW.size

    def rows(self, start, stop):
        return _ROW.iter_unpack(memoryview(self._data)[start * _ROW.size : stop * _ROW.size])

    def __iter__(self):
        for x, y, nx, ny, z, power, start in self.rows(0, len(self)):
            yield [x, y, nx, ny, z, None if math.isnan(power) else power, start]

    def buffer(self):
        # Freeze at the native boundary so the GIL can be released safely.
        if isinstance(self._data, bytearray):
            self._data = bytes(self._data)
        return self._data

    def extend(self, other):
        if isinstance(other, PackedSegments):
            self._writable().extend(other._data)
        else:
            for row in other:
                self.append(row)


@dataclass
class CutRun:
    """Contiguous generated G1 rows with the same modal feed and word layout."""

    segments: PackedSegments
    start: int
    stop: int
    feed: float
    variable_z: bool
    laser: bool

    def blocks(self):
        # The reference post reads planar X/Y/Z to 3 decimals, F to 1, S to 0.
        # Quantize once here so wrapped output follows the identical machine path.
        feed = round(self.feed, 1)
        for _, _, x, y, z, power, _ in self.segments.rows(self.start, self.stop):
            words = {'G': 1.0, 'X': round(x, 3), 'Y': round(y, 3), 'F': feed}
            if self.variable_z:
                words['Z'] = round(z, 3)
            if self.laser:
                words['S'] = round(power, 0)
            yield 'G1', words, None


def block_count(lines):
    return sum(item.stop - item.start if isinstance(item, CutRun) else 1 for item in lines)
