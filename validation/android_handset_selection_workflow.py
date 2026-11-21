"""Select overlapping shapes from the Pocket drawing preview on a physical handset.

    python validation/android_handset_selection_workflow.py --serial 192.168.1.50:5555

Drives the debug build (see pocket_app.py) on a supervised, unlocked handset. The run empties the active
drawing of that build, adds three default rectangles, which overlap, and taps the preview:

* a corner covered by one shape selects that shape;
* a point covered by two shapes selects the upper one, and a second tap there selects the lower one;
* three taps at the common centre reach all three shapes;
* a tap clear of every shape leaves the selection as it was.

The selected shape is read from the objects list. Nothing is sent to a machine. The three rectangles are left
in the debug build's active drawing.
"""

import argparse
import re
import time

from android_handset_pendant_workflow import LAUNCH, Phone, step, wait_until
from pocket_app import PACKAGE

SHAPE = 'Rectangle · 40.0 × 25.0 mm'
PREVIEW = 'Drawing preview. Select a shape here or in the objects list.'
PREVIEW_HEIGHT_DP = 205
# Default corners of three successive new shapes, each 40 × 25 mm.
CORNERS = [(10, 10), (15, 15), (20, 20)]


def bounds(node):
    left, top, right, bottom = (int(value) for value in re.findall(r'\d+', node.get('bounds')))
    return left, top, right, bottom


def shapes(phone):
    """The objects list in drawing order: (centre point, selected) for each shape button."""
    rows = []
    for node in phone.tree().iter('node'):
        if (
            node.get('class') == 'android.widget.Button'
            and ' mm' in (node.get('text') or '')
            and '·' in node.get('text')
        ):
            left, top, right, bottom = bounds(node)
            rows.append((((left + right) // 2, (top + bottom) // 2), node.get('selected') == 'true'))
    return rows


def selected(phone):
    marks = [index for index, (_, chosen) in enumerate(shapes(phone)) if chosen]
    assert len(marks) <= 1, 'More than one shape is marked selected.'
    return marks[0] if marks else -1


def empty(phone):
    """Remove every shape from the active drawing of the debug build."""
    for _ in range(140):
        rows = shapes(phone)
        if not rows:
            return
        if not any(chosen for _, chosen in rows):
            phone.run('shell', 'input', 'tap', *(str(v) for v in rows[0][0]))
        phone.tap('Delete', button=True)
        phone.confirm()
        count = len(rows)
        wait_until(lambda count=count: len(shapes(phone)) < count, 'A shape was not deleted.')
    raise AssertionError('The drawing could not be emptied.')


def exercise(phone):
    assert phone.run('shell', 'pm', 'list', 'packages', PACKAGE), f'Install the Pocket build {PACKAGE} first.'
    density = int(re.findall(r'(\d+)', phone.run('shell', 'wm', 'density'))[-1]) / 160
    phone.run('shell', 'am', 'force-stop', PACKAGE)
    phone.run(*LAUNCH)
    wait_until(lambda: phone.find('Add shape', button=True), 'The editor did not open.')
    try:
        empty(phone)

        def add():
            for count in range(1, 4):
                phone.tap('Add shape', button=True)
                phone.confirm()
                wait_until(lambda count=count: len(shapes(phone)) == count, 'The shape was not added.')
            assert selected(phone) == 2, 'The newest shape should be selected.'

        step('add three default rectangles, which overlap', add)

        view = next(node for node in phone.tree().iter('node') if node.get('content-desc') == PREVIEW)
        left, top, right, bottom = bounds(view)
        width, height = right - left, bottom - top
        assert abs(height - round(PREVIEW_HEIGHT_DP * density)) <= 1, 'Scroll the drawing preview fully into view.'
        # The preview fits the shapes' bounds inside a 12% margin (BedView.onDraw).
        min_x, min_y = CORNERS[0]
        max_x, max_y = CORNERS[-1][0] + 40, CORNERS[-1][1] + 25
        padding = min(width, height) * 0.12
        scale = min((width - 2 * padding) / (max_x - min_x), (height - 2 * padding) / (max_y - min_y))
        origin_x = left + width / 2 - scale * (min_x + max_x) / 2
        origin_y = top + height / 2 + scale * (min_y + max_y) / 2
        print(
            f'Preview {width} × {height} px at {scale:.2f} px/mm; tap tolerance {12 * density / scale:.1f} mm.',
            flush=True,
        )

        def tap(x, y, expected, what):
            phone.run('shell', 'input', 'tap', str(round(origin_x + x * scale)), str(round(origin_y - y * scale)))
            time.sleep(0.4)
            found = selected(phone)
            assert found == expected, f'{what}: shape {expected + 1} expected, shape {found + 1} selected.'

        def single():
            tap(12.5, 12.5, 0, 'Corner covered by the first shape only')
            tap(57.5, 42.5, 2, 'Corner covered by the third shape only')

        step('a corner covered by one shape selects it', single)

        def pair():
            tap(17.5, 17.5, 1, 'Point covered by the first and second shapes')
            tap(17.5, 17.5, 0, 'Second tap at the same point')
            tap(17.5, 17.5, 1, 'Third tap at the same point')

        step('repeated taps where two shapes overlap alternate between them', pair)

        def stack():
            reached = set()
            for _ in range(3):
                phone.run(
                    'shell', 'input', 'tap', str(round(origin_x + 35 * scale)), str(round(origin_y - 27.5 * scale))
                )
                time.sleep(0.4)
                reached.add(selected(phone))
            assert reached == {0, 1, 2}, (
                f'Three taps at the common centre reached shapes {sorted(i + 1 for i in reached)}.'
            )

        step('three taps at the common centre reach all three shapes', stack)

        def clear():
            before = selected(phone)
            phone.run('shell', 'input', 'tap', str(left + 6), str(top + 6))
            time.sleep(0.4)
            assert selected(phone) == before, 'A tap clear of every shape changed the selection.'

        step('a tap clear of every shape keeps the selection', clear)
    finally:
        phone.cleanup()
    print('PASS overlapping shapes can each be selected from the drawing preview on a physical handset.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--adb', default='adb', help='ADB executable')
    parser.add_argument('--serial', required=True, help='ADB serial of the supervised handset')
    args = parser.parse_args()
    exercise(Phone(args.adb, args.serial))
