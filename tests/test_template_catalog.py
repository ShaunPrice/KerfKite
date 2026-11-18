import json
import struct
import tempfile
import unittest
import zlib
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from unittest.mock import patch

from studio.template_catalog import TemplateCatalog, _load_index


def replace_catalog(path, records):
    data = bytearray(b"KKART\x01\r\n")
    entries = []
    for key, name, endpoint in records:
        blob = zlib.compress(struct.pack("<II4d", 1, 2, 0, 0, endpoint, endpoint))
        entries.append([len(data), len(blob), {"id": key, "name": name, "category": "Test", "aspect": 1}])
        data.extend(blob)
    index = json.dumps(entries).encode()
    data.extend(index)
    data.extend(struct.pack("<I", len(index)))
    pending = path.with_suffix(".tmp")
    pending.write_bytes(data)
    pending.replace(path)


class CatalogUpdateTests(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.path = Path(tmp.name) / "art.kkart"
        replace_catalog(self.path, [("a", "First", 3), ("b", "Second", 2)])

    def test_replacement_refreshes_metadata_offsets_and_releases_paused_iterator(self):
        catalog = TemplateCatalog(self.path)
        items = iter(catalog)
        self.assertEqual(next(items)["id"], "a")
        replace_catalog(self.path, [("b", "Second", 2), ("a", "Revised", 5), ("c", "Third", 4)])
        self.assertEqual(catalog.get("a")["paths"][0][-1], [5, 5])
        self.assertEqual(catalog.search("revised")[0]["id"], "a")
        self.assertEqual([item["id"] for item in catalog[:]], ["b", "a", "c"])
        self.assertEqual(len(catalog), 3)
        with self.assertRaisesRegex(RuntimeError, "changed during export"):
            next(items)
        self.assertEqual([item["id"] for item in catalog], ["b", "a", "c"])

    def test_concurrent_lookups_return_independent_geometry_and_reuse_the_index(self):
        with patch("studio.template_catalog._load_index", wraps=_load_index) as load:
            catalog = TemplateCatalog(self.path)

            def lookup(key):
                item = catalog.get(key)
                point = item["paths"][0][-1]
                endpoint = 3 if key == "a" else 2
                self.assertEqual((item["id"], point), (key, [endpoint, endpoint]))
                point[0] = -1

            with ThreadPoolExecutor(max_workers=8) as pool:
                list(pool.map(lookup, ["a", "b"] * 100))
            self.assertEqual(load.call_count, 1)
            replace_catalog(self.path, [("b", "Second", 2)])
            self.assertIsNone(catalog.get("a"))
            self.assertEqual(catalog[0]["id"], "b")
            self.assertEqual(load.call_count, 2)
