# Optional C++ relief backend

`studio/_relief_native.cpp` is an in-process CPython extension. It accelerates conservative flat/ball cutter contact; Python retains input validation, work caps, path construction, metadata and the reference implementation. There are no runtime compiler or subprocess calls. The kernel releases the GIL during its bounded numerical loop.

Install with a compiler and Python headers, then `python -m pip install .`. Developers can use `python setup.py build_ext --inplace`. Windows uses MSVC `/O2`; other builds use `-O3 -std=c++11`, without fast-math. The extension is optional, so inspect the backend rather than assuming compilation succeeded.

```python
from studio.relief import prepare_relief
result = prepare_relief(data)  # auto: native if importable, otherwise Python
print(result['surface']['backend'])  # cpp or python
reference = prepare_relief(data, backend='python')
accelerated = prepare_relief(data, backend='native')  # explicit error if absent
```

The UI identifies accelerated relief results. Restart an already-running server after installing the extension. Browser-only file mode does not load a CPython extension.

Native and Python parity tests cover planes, ramps, domes, peaks, random fields, both cutter shapes, allowances, malformed input and the fallback path. The installed Pi wheel passed ten relief/native tests; six complete benchmark jobs produced the same G-code hash. The original standalone C++ executable remains only as a historical benchmark fixture.

Installed Pi medians: 1.142 s Python relief versus 0.086 s C++; 2.864 s total versus 1.818 s. About 37% less total calculation time for that fixture. C++ was chosen because its implementation was directly measured; no Rust performance comparison was performed. Stock preview is the largest remaining stage.

Evidence: `validation/native-installed-pi5-2026-09-11.json` and `validation/native-installed-mac-2026-09-11.json`. The Mac evidence exercises the local compiled module; the Pi evidence imports from the temporary environment's installed site-packages. No persistent Pi service was deployed.
