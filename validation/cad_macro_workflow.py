"""Run CAD macros through the browser and measure the resulting geometry."""

import json
import math
from pathlib import Path
import re

from playwright.sync_api import expect


COMMANDS = "# Registration border\nrect {width} 6 1 2\nline 1 5 $width 5\nfit drawing"
MODEL = "translate([offset, 0, 0]) cube([width, 6, 3]);"


class Workshop:
    def __init__(self, page):
        self.page = page
        self.loc = page.locator
        self.dialog = self.loc('#cad-macro-dialog')
        self.list = self.loc('#cad-macro-list')
        self.status = self.loc('#cad-macro-status')
        self.run_button = self.loc('#cad-macro-run')
        self.ready()

    def ready(self):
        self.page.evaluate('() => window.studioReady')
        self.page.wait_for_function('window.KerfKiteMacros && window.CNCProjects')

    def open(self):
        if not self.dialog.is_visible():
            self.loc('#cad-macros-open').click()
        expect(self.dialog).to_be_visible()

    def close(self):
        if self.dialog.is_visible():
            self.loc('#cad-macro-close').click()
        expect(self.dialog).not_to_be_visible()

    def select(self, name):
        self.open()
        self.list.select_option(label=name)
        expect(self.loc('#cad-macro-name')).to_have_value(name)

    def define(self, name, kind, source, parameters):
        self.open()
        self.loc('#cad-macro-new').click()
        self.loc('#cad-macro-name').fill(name)
        self.loc('#cad-macro-kind').select_option(kind)
        self.loc('#cad-macro-source').fill(source)
        for key, value in parameters.items():
            self.loc('#cad-macro-param-add').click()
            self.loc('.macro-param-name').last.fill(key)
            self.loc('.macro-param-default').last.fill(str(value))
        self.loc('#cad-macro-save').click()
        expect(self.list.locator('option:checked')).to_have_text(name)

    def command(self, instruction):
        self.close()
        field = self.loc('#cad-command')
        field.fill(instruction)
        field.press('Enter')
        expect(field).to_have_value('')

    def new_project(self):
        self.close()
        self.page.get_by_role('tab', name='Projects', exact=True).click()
        self.loc('#projectNew').click()
        self.page.wait_for_function(
            'CNCProjects.document().objects.length === 0 && !CNCSolidExchange.model().features.length'
        )

    def objects(self):
        return self.page.evaluate('CNCProjects.document().objects')

    def feature_model(self):
        return self.page.evaluate('CNCSolidExchange.model()')

    def sizes(self):
        return self.page.evaluate('CNCProjects.document().objects.map(CNCLayout.extent)')

    def export(self):
        self.open()
        with self.page.expect_download() as file:
            self.loc('#cad-macro-export').click()
        return json.loads(Path(file.value.path()).read_bytes())

    def upload(self, text):
        self.loc('#cad-macro-file').set_input_files(
            {'name': 'fixtures.kerfkite-macros.json', 'mimeType': 'application/json', 'buffer': text.encode()}
        )


def command_recipe(workshop):
    workshop.define('Border', 'commands', COMMANDS, {'width': 18})
    workshop.select('Border')
    workshop.loc('.macro-param-run').fill('24')
    workshop.run_button.click()
    workshop.page.wait_for_function('CNCProjects.document().objects.length === 2')
    assert workshop.sizes() == [[1, 2, 25, 8], [1, 5, 24, 5]]
    workshop.command('macro Border width=32')
    assert workshop.sizes()[2:] == [[1, 2, 33, 8], [1, 5, 32, 5]]
    workshop.command('macro Border')
    assert workshop.sizes()[4:] == [[1, 2, 19, 8], [1, 5, 18, 5]]

    # The malformed last command must leave both artwork and camera untouched.
    workshop.select('Border')
    objects = workshop.objects()
    camera = workshop.page.evaluate("KerfKiteViews.state('2d')")
    workshop.loc('#cad-macro-source').fill('rect 5 6\npan 35 0\ncircle -1')
    workshop.run_button.click()
    expect(workshop.status).to_contain_text(re.compile(r'line 3', re.I))
    assert workshop.objects() == objects
    assert workshop.page.evaluate("KerfKiteViews.state('2d')") == camera
    workshop.close()


def solid_recipe(workshop):
    workshop.define('Strip', 'scad', MODEL, {'width': 12, 'offset': 0})
    workshop.command('macro Strip width=20')
    expect(workshop.loc('#solid-size')).to_have_text('20 × 6 × 3')
    expect(workshop.loc('#solid-volume')).to_have_text(re.compile(r'^360(?:\.0+)?$'))
    workshop.command('macro Strip offset=40')
    expect(workshop.loc('#solid-size')).to_have_text('52 × 6 × 3')
    expect(workshop.loc('#solid-volume')).to_have_text(re.compile(r'^576(?:\.0+)?$'))
    before = workshop.feature_model()
    assert [(f['source'], f['scriptParameters']) for f in before['features']] == [
        (MODEL, {'width': 20, 'offset': 0}),
        (MODEL, {'width': 12, 'offset': 40}),
    ]
    field = workshop.loc('#cad-command')
    field.fill('macro Strip width=-1')
    field.press('Enter')
    expect(workshop.loc('#cad-command-result')).to_contain_text(re.compile(r'positive', re.I))
    assert workshop.feature_model() == before
    field.fill('macro Strip offset=abc')
    field.press('Enter')
    expect(workshop.loc('#cad-command-result')).to_contain_text(re.compile(r'number|numeric|finite', re.I))
    assert workshop.feature_model() == before


def library_exchange(workshop):
    original = workshop.export()
    assert original['format'] == 'kerfkite-cad-macros' and original['version'] == 1
    recipes = {recipe['name']: recipe for recipe in original['macros']}
    assert (recipes['Border']['source'], recipes['Border']['parameters']) == (COMMANDS, {'width': 18})
    assert recipes['Strip']['source'] == MODEL and recipes['Strip']['parameters'] == {'width': 12, 'offset': 0}
    workshop.close()
    workshop.page.reload()
    workshop.ready()
    workshop.select('Border')
    expect(workshop.loc('#cad-macro-source')).to_have_value(COMMANDS)
    workshop.loc('#cad-macro-delete').click()
    expect(workshop.list.locator('option', has_text='Border')).to_have_count(0)
    workshop.upload(json.dumps(original))
    expect(workshop.list.locator('option', has_text='Border')).to_have_count(1)
    restored = {recipe['name']: recipe for recipe in workshop.export()['macros']}
    assert restored == recipes

    # Identical definitions merge even after a second import. An invalid file changes nothing.
    workshop.upload(json.dumps(original))
    expect(workshop.list.locator('option')).to_have_count(len(recipes))
    workshop.upload('{')
    expect(workshop.status).to_contain_text(re.compile(r'json|invalid|expected', re.I))
    assert {r['name']: r for r in workshop.export()['macros']} == recipes
    another = {**recipes['Strip'], 'source': 'cube([width, 9, 2]);'}
    workshop.upload(json.dumps({**original, 'macros': [another]}))
    expect(workshop.list.locator('option')).to_have_count(len(recipes) + 1)
    merged = workshop.export()['macros']
    assert any(r['name'] != 'Strip' and r['source'] == another['source'] for r in merged)
    assert next(r for r in merged if r['name'] == 'Strip') == recipes['Strip']
    workshop.upload(json.dumps({**original, 'macros': [another]}))
    expect(workshop.status).to_contain_text('no new macros')
    expect(workshop.list.locator('option')).to_have_count(len(recipes) + 1)
    workshop.close()


def run(page):
    errors = []
    page.on('pageerror', lambda exc: errors.append(str(exc)))
    workshop = Workshop(page)
    command_recipe(workshop)
    solid_recipe(workshop)
    library_exchange(workshop)
    starter_recipes(workshop)
    assert not errors, errors
    print(
        'PASS CAD macro authoring, repeat runs, atomic validation, solid measurements, '
        'browser persistence, library import/export and all starter recipes'
    )


def starter_recipes(workshop):
    # Use the facet counts from the recipes to distinguish through-bores from a solid blank.
    area48 = 24 * math.sin(math.tau / 48)
    area64 = 32 * math.sin(math.tau / 64)
    solids = {
        'Drilled spacer': ('18 × 18 × 8', area64 * (9**2 - 3**2) * 8),
        'Mounting plate': ('70 × 40 × 5', 70 * 40 * 5 - 4 * area48 * 2**2 * 5),
        'Flanged bushing': ('22 × 22 × 16', area64 * ((7**2 - 4**2) * 16 + (11**2 - 7**2) * 3)),
    }
    objects = {
        'Drawing frame': [[0, 0, 60, 30]],
        'Fastener layout': [[6, 6, 10, 10], [50, 6, 54, 10], [50, 26, 54, 30], [6, 26, 10, 30]],
        'Rounded tag': [[0, 0, 60, 24]],
        'Slotted panel': [[0, 0, 80, 40]],
    }
    # A fresh library is seeded with exactly these examples. Future additions need a measured fixture.
    workshop.open()
    starter_names = {m['name'] for m in workshop.export()['macros']} - {'Border', 'Strip', 'Strip (2)'}
    assert starter_names == set(solids) | set(objects), starter_names
    for name in sorted(starter_names):
        workshop.new_project()
        workshop.select(name)
        workshop.run_button.click()
        if name in solids:
            size, volume = solids[name]
            expect(workshop.loc('#solid-size')).to_have_text(size)
            expect(workshop.loc('#solid-status')).to_have_text('Solid ready.')
            actual = float(workshop.loc('#solid-volume').inner_text().replace(',', ''))
            assert math.isclose(actual, volume, abs_tol=0.02), (name, actual, volume)
        else:
            workshop.page.wait_for_function('n => CNCProjects.document().objects.length === n', arg=len(objects[name]))
            assert workshop.sizes() == objects[name], name
            if name in ('Rounded tag', 'Slotted panel'):
                paths = workshop.objects()[0]['paths']
                spans = sorted(
                    (
                        round(max(x for x, _ in path) - min(x for x, _ in path), 3),
                        round(max(y for _, y in path) - min(y for _, y in path), 3),
                    )
                    for path in paths
                )
                expected = [(5, 5), (60, 24)] if name == 'Rounded tag' else [(34, 3)] * 4 + [(80, 40)]
                assert spans == expected, (name, spans)
    workshop.close()
