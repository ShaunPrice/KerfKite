// Run in a fresh process: node --expose-gc validation/benchmark_template_catalog.cjs
// Measures JS catalogue allocation only. A real browser also allocates the SVG DOM.
'use strict';
const {performance} = require('node:perf_hooks');
if (!global.gc) throw new Error('Run with node --expose-gc for retained heap measurements.');
function report(phase, start) {
    const elapsed = performance.now() - start;
    global.gc();
    const memory = process.memoryUsage();
    console.log(JSON.stringify({
        phase, milliseconds: Math.round(elapsed),
        heapMiB: +(memory.heapUsed / 1048576).toFixed(1),
        rssMiB: +(memory.rss / 1048576).toFixed(1)
    }));
}
let start = performance.now();
const {templates} = require('../studio/static/templates.js');
report('load', start);
start = performance.now();
const categories = new Set(templates.map(template => template.category));
if (categories.size < 100) throw new Error('Expected the expanded stock catalogue');
report('browse metadata', start);
start = performance.now();
let pointCount = 0;
for (const template of templates.slice(0, 36)) {
    for (const path of template.paths) pointCount += path.length;
}
if (pointCount !== 94848) throw new Error('First page geometry changed');
report('open first 36 geometries', start);
console.log(JSON.stringify({templates: templates.length, categories: categories.size, pointCount}));
