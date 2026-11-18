const assert = require('node:assert/strict');
const test = require('node:test');
const catalogue = require('../../studio/static/materials-data.js');

const { materials, sources, tools } = catalogue;
const ratings = new Set(['candidate', 'conditional', 'avoid']);

function requireNames(items) {
    const names = items.map(item => item.id);
    assert.ok(names.every(Boolean), 'Every item needs an ID');
    assert.equal(new Set(names).size, items.length, 'IDs must be unique');
}

test('the guide covers all installed head and cutter types', () => {
    requireNames(tools);
    for (const id of ['flat', 'ball', 'drill', 'v', 'diode', 'knife']) {
        assert.ok(tools.some(tool => tool.id === id), id);
    }
    for (const tool of tools) {
        assert.ok(tool.label && tool.family && tool.operations.length, tool.id);
        requireNames(tool.operations);
        assert.ok(tool.operations.every(operation => operation.label), tool.id);
    }
});

test('the material matrix has a documented verdict for every material and tool', () => {
    requireNames(materials);
    const toolIds = tools.map(tool => tool.id).sort();
    for (const material of materials) {
        const { id, name, group, description, aliases } = material;
        assert.ok(name && group && description && Array.isArray(aliases), id);
        assert.deepEqual(Object.keys(material.tools).sort(), toolIds, id);
        for (const tool of tools) {
            const advice = material.tools[tool.id];
            const key = [id, tool.id].join('/');
            assert.ok(ratings.has(advice.status) && advice.summary, key);
            for (const field of ['setup', 'inspect', 'sources']) {
                assert.ok(Array.isArray(advice[field]) && advice[field].length, key + '/' + field);
            }
            assert.deepEqual(Object.keys(advice.operations).sort(), tool.operations.map(op => op.id).sort(), key);
            for (const [opId, override] of Object.entries(advice.operations)) {
                assert.ok(ratings.has(override.status) && override.advice, key + '/' + opId);
            }
            for (const sourceId of advice.sources) {
                const ref = sources[sourceId];
                assert.ok(ref?.title && new URL(ref.url).protocol === 'https:', key + '/' + sourceId);
            }
        }
    }
});

function verdict(materialId, toolId, operationId) {
    const material = materials.find(item => item.id === materialId);
    assert.ok(material, materialId);
    const assessment = material.tools[toolId];
    assert.ok(tools.find(tool => tool.id === toolId).operations.some(op => op.id === operationId));
    return assessment.operations?.[operationId]?.status ?? assessment.status;
}

test('PVC sign vinyl can be kiss-cut with a blade, and both diode operations are blocked', () => {
    assert.equal(verdict('pvc-vinyl', 'knife', 'kiss'), 'candidate');
    for (const op of ['engrave', 'cut']) {
        assert.equal(verdict('pvc-vinyl', 'diode', op), 'avoid');
    }
});

test('the blue-diode guide distinguishes clear acrylic and marking anodised metal from cutting it', () => {
    for (const op of ['engrave', 'cut']) {
        assert.equal(verdict('clear-acrylic', 'diode', op), 'avoid');
    }
    assert.equal(verdict('anodised-aluminium', 'diode', 'engrave'), 'conditional');
    assert.equal(verdict('anodised-aluminium', 'diode', 'cut'), 'avoid');
});

test('drill guide distinguishes supported stock, test-required sheet and unknown material',()=>{
    assert.equal(verdict('maple','drill','drill'),'candidate');
    assert.equal(verdict('unknown','drill','peck'),'avoid');
    const acrylic=materials.find(m=>m.id.includes('acrylic'));
    assert.ok(acrylic.tools.drill.sources.includes('acrylicDrill'));
    assert.equal(acrylic.tools.drill.operations.peck.status,'conditional');
});
