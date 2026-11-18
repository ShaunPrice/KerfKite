"""Analytic acceptance cases through Studio's HTTP API on the simulator."""
import base64
import math


def assembly(client):
    from studio.assembly import example_assembly
    doc = example_assembly()
    good = client.request('assembly/solve', {'assembly': doc})
    assert not good['interferences'] and len(good['parts']) == 2
    doc['mates'][0]['parentFrame']['origin'][0] += 4
    bad = client.request('assembly/solve', {'assembly': doc})
    assert len(bad['interferences']) == 1 and bad['interferences'][0]['volume'] > 0
    return {'parts': len(good['parts']), 'offset_mm': 4, 'overlap_mm3': bad['interferences'][0]['volume']}


SURFACE = {'poles': [[[0, 0, 0], [0, 12, 4]], [[20, 0, 4], [20, 12, 0]]]}


def surface(client):
    mesh = client.request('solid-cad/surface', {'surface': SURFACE})
    middle = mesh['vertices'][len(mesh['vertices'])//2]
    assert middle == [10, 6, 2] and mesh['area'] > 240 and mesh['solids'] == 0
    return {'centre_mm': middle, 'area_mm2': mesh['area']}


def step(client):
    for key, doc, solids in [('surface', SURFACE, 0), ('model', {
            'version': 1, 'features': [{'id': 'pin', 'kind': 'cylinder','radius': 3,'height': 7}]}, 1)]:
        raw = client.request('solid-cad/step/export', {key: doc}, binary=True)
        result = client.request('solid-cad/step/import', {'content': base64.b64encode(raw).decode()})
        assert raw.startswith(b'ISO-10303-21;') and result['solids'] == solids
        assert result['triangles'] and result['faces'] >= 1
        if solids:
            assert math.isclose(result['volume'], 63*math.pi, rel_tol=1e-6)
    return {'open_patch_faces': 1, 'cylinder_faces': result['faces'], 'volume_mm3': result['volume']}


def collision(client):
    data = {'mode': 'router', 'power': 8000, 'width': 100, 'height': 60, 'step': 1,
            'depth': 1, 'feed': 500, 'safe': 4, 'paths': [[[5, 20], [10, 20]], [[50, 20], [55, 20]]]}
    lines = client.request('generate', data)['lines']
    clamp = {'name': 'Fixture', 'min': [30, 21, 7], 'max': [32, 23, 10]}
    setup = {'lines': lines, 'fixtures': [clamp], 'start': [0, 0, 4],
             'tool': {'diameter': 2, 'stickout': 4, 'holder_diameter': 12, 'holder_length': 15}}
    hit = client.request('fixture-collision', setup)
    assert any(e['component']=='holder' and e['motion']=='G0' for e in hit['events'])
    clamp.update(min=[30, 40, 7], max=[32, 42, 10])
    clear = client.request('fixture-collision', setup)
    assert not clear['collision'] and hit['moves_checked'] == clear['moves_checked']
    return {'moves': clear['moves_checked'], 'rapid_holder_contact': True, 'relocated_clamp_clear': True}


def program_import(client):
    result = client.request('program-import', {'name': 'motif.plt', 'text':'IN;SP1;PU400,400;PD800,400,800,800;PU;'})
    paths = [p for obj in result['objects'] for p in obj['paths']]
    assert paths == [[[10,10], [20,10], [20,20]]]
    job = client.request('generate', {'mode': 'knife', 'paths': paths, 'width': 100, 'height': 100,
                         'depth': .2,'step': .2,'power': 1,'feed': 200,'knife_offset': .25})
    assert job['segments'] and not any(s.startswith(('M3','M4')) for s in job['lines'])
    return {'knife_segments': len(job['segments'])}
