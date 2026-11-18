// Stock preview v1. Keep machining coordinates as float64 through transport.
(function (root) {
  'use strict';
  const mime = 'application/vnd.kerfkite.stock-v1';
  const shapes = ['flat', 'ball', 'v', 'drill'];

  function encode(data) {
    const rows = data.segments;
    if (!Array.isArray(rows) || rows.length > 200000) throw Error('Invalid segment count');
    const shape = shapes.indexOf(data.tool_shape ?? 'flat');
    if (shape < 0) throw Error('Unknown simulated cutter');
    const resolution = data.resolution ?? 48;
    if (!Number.isFinite(resolution) || resolution < 16 || resolution > 96) throw Error('Invalid stock preview resolution');
    const bytes = new ArrayBuffer(76 + 56 * rows.length);
    const view = new DataView(bytes);
    new Uint8Array(bytes, 0, 8).set([75,75,83,84,79,67,75,49]); // KKSTOCK1
    view.setUint32(8, rows.length, true);
    view.setUint32(12, Math.trunc(resolution), true);
    view.setUint32(16, shape, true);
    let offset = 20;
    function write(value) {
      if (!Number.isFinite(value)) throw Error('Stock geometry and dimensions must be finite numbers');
      view.setFloat64(offset, value, true);
      offset += 8;
    }
    for (const value of [data.x ?? 0, data.y ?? 0, data.width ?? 100, data.height ?? 100,
                         data.tool_diameter ?? 3.175, data.v_angle ?? 60, data.stock_thickness ?? 10]) write(value);
    for (const row of rows) {
      if (!Array.isArray(row) || row.length < 5) throw Error('Invalid preview segment');
      for (let index = 0; index < 5; index++) write(row[index]);
      // The stock kernel ignores the power slot of the common packed CAM row.
      view.setFloat64(offset, NaN, true);
      offset += 8;
      write(row.length > 6 ? row[6] : row[4]);
    }
    return bytes;
  }
  const api = {mime, encode};
  root.KerfKiteStockWire = api;
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
})(globalThis);
