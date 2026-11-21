"""Random access to KerfKite's bundled vector artwork; see scripts/build_templates.cjs."""

import json
import os
import struct
import zlib
from collections.abc import Sequence
from contextlib import contextmanager
from dataclasses import dataclass
from itertools import islice
from pathlib import Path


_CATALOG = Path(__file__).with_name("builtin_templates.kkart")
_MAGIC = b"KKART\x01\r\n"
_UINT = struct.Struct("<I")
_POINT = struct.Struct("<dd")


def _paths(compressed):
    data = memoryview(zlib.decompress(compressed))
    (count,) = _UINT.unpack_from(data)
    offset = _UINT.size
    paths = []
    for _ in range(count):
        (points,) = _UINT.unpack_from(data, offset)
        offset += _UINT.size
        end = offset + points * _POINT.size
        if end > len(data):
            raise ValueError("Truncated template path")
        paths.append([list(point) for point in _POINT.iter_unpack(data[offset:end])])
        offset = end
    if offset != len(data):
        raise ValueError("Unexpected data after template paths")
    return paths


@dataclass(frozen=True)
class _Index:
    identity: tuple
    entries: list
    positions: dict


def _load_index(stream, identity):
    if stream.read(len(_MAGIC)) != _MAGIC:
        raise ValueError("Unsupported artwork catalog format")
    index_end = stream.seek(-_UINT.size, 2)
    (index_size,) = _UINT.unpack(stream.read(_UINT.size))
    index_start = index_end - index_size
    if index_start < len(_MAGIC):
        raise ValueError("Invalid artwork catalog index")
    stream.seek(index_start)
    entries = json.loads(stream.read(index_size))
    return _Index(identity, entries, {entry[2]["id"]: i for i, entry in enumerate(entries)})


class TemplateCatalog(Sequence):
    """A searchable, ordered catalog. Each geometry lookup yields independent paths."""

    def __init__(self, path=_CATALOG):
        self.path = Path(path)
        self._index = None
        self._current_index()

    @contextmanager
    def _open(self):
        # An installer or recipe rebuild can atomically replace the file while
        # MCP is running. Always pair offsets with the handle they describe.
        with self.path.open("rb") as stream:
            stat = os.fstat(stream.fileno())
            identity = (stat.st_dev, stat.st_ino, stat.st_size, stat.st_mtime_ns, stat.st_ctime_ns)
            index = self._index
            if index is None or index.identity != identity:
                index = _load_index(stream, identity)
                self._index = index
            yield stream, index

    def _current_index(self):
        with self._open() as (_, index):
            return index

    def __len__(self):
        return len(self._current_index().entries)

    def _read(self, stream, entry):
        offset, length, metadata = entry
        stream.seek(offset)
        return {**metadata, "paths": _paths(stream.read(length))}

    def __getitem__(self, position):
        with self._open() as (stream, index):
            if isinstance(position, slice):
                return [self._read(stream, entry) for entry in index.entries[position]]
            return self._read(stream, index.entries[position])

    def __iter__(self):
        snapshot = self._current_index()
        for entry in snapshot.entries:
            # Close before yielding so a paused iterator cannot lock an update
            # on Windows. A single export must use one version throughout.
            with self._open() as (stream, index):
                if index.identity != snapshot.identity:
                    raise RuntimeError("Artwork catalog changed during export. Retry the export.")
                item = self._read(stream, entry)
            yield item

    def get(self, identifier):
        with self._open() as (stream, index):
            position = index.positions.get(identifier) if isinstance(identifier, str) else None
            return self._read(stream, index.entries[position]) if position is not None else None

    def search(self, query="", category="", limit=30):
        query = query.casefold()
        entries = self._current_index().entries
        metadata = (
            item
            for _, _, item in entries
            if (not category or item["category"] == category)
            and query in (item["name"] + " " + item["category"]).casefold()
        )
        return [
            {key: item[key] for key in ("id", "name", "category")} for item in islice(metadata, max(1, min(100, limit)))
        ]
