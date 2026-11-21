/* The phone pairing link as a QR symbol for Pocket's scanner. Geometry only: no page access. */
((root) => {
  'use strict';
  const QUIET = 4;
  // Dark modules by row, without the quiet zone. Medium error correction: the code is read from a screen.
  function modules(link) {
    if (typeof link !== 'string' || link.length > 400 || !/^https?:\/\/[^\s#]+\/#token=[^\s]+$/.test(link)) throw Error('Not a pairing link.');
    const {QrCode} = root.qrcodegen;
    const code = QrCode.encodeText(link, QrCode.Ecc.MEDIUM);
    return Array.from({length:code.size}, (_, y) => Array.from({length:code.size}, (_, x) => code.getModule(x, y)));
  }
  // One SVG path of horizontal runs, in module units, offset by the quiet zone.
  function symbol(link) {
    const rows = modules(link);
    let path = '';
    rows.forEach((row, y) => {
      for (let x = 0; x < row.length; x++) {
        if (!row[x]) continue;
        let end = x;
        while (end + 1 < row.length && row[end + 1]) end++;
        path += 'M' + (x + QUIET) + ',' + (y + QUIET) + 'h' + (end - x + 1) + 'v1h-' + (end - x + 1) + 'z';
        x = end;
      }
    });
    return {size:rows.length + 2 * QUIET, quiet:QUIET, path, rows};
  }
  // Black on white whatever the page colours: scanners expect a light quiet zone.
  function svg(link) {
    const {size, path} = symbol(link);
    return '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 ' + size + ' ' + size + '" shape-rendering="crispEdges" style="display:block;width:100%;height:auto">' +
      '<rect width="' + size + '" height="' + size + '" fill="#fff"/><path d="' + path + '" fill="#000"/></svg>';
  }
  root.KerfPairingQr = {modules, symbol, svg};
})(globalThis);
