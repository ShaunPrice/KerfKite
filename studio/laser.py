"""Vector laser lead geometry, distance-based power ramps and beam-off intervals."""
from bisect import bisect_left, bisect_right
import math


def cut_options(project, number):
    pattern = project.get('laser_pattern', 'continuous')
    shape = project.get('lead_shape', 'straight')
    if pattern not in ('continuous', 'perforate', 'bridges'):
        raise ValueError('Choose continuous cutting, perforation or holding bridges')
    if shape not in ('straight', 'arc-left', 'arc-right'):
        raise ValueError('Choose a straight, left or right tangent lead')
    if pattern != 'continuous' and (project.get('path_powers') is not None or
                                   project.get('operation', 'line') not in ('line', 'inside', 'outside')):
        raise ValueError('Laser perforation and bridges require vector line or profile operations')
    options = dict(pattern=pattern, shape=shape)
    if pattern != 'continuous':
        options['gap'] = number(project.get('laser_gap', 1), 'Beam-off gap (mm)', .01, 100)
    if pattern == 'perforate':
        options['dash'] = number(project.get('laser_dash', 3), 'Perforation cut (mm)', .01, 1000)
    if pattern == 'bridges':
        count = number(project.get('laser_bridge_count', 4), 'Bridges per contour', 1, 1000)
        if not count.is_integer():
            raise ValueError('Bridges per contour must be a whole number')
        options['count'] = int(count)
    return options


def _lead(a, b, length, shape, entering):
    """Additional points ordered in travel direction. Arc length is the UI dimension."""
    if length < 1e-9:
        return []
    edge = math.dist(a, b)
    tx, ty = (b[0]-a[0])/edge, (b[1]-a[1])/edge
    x, y = a if entering else b
    sign = -1 if entering else 1
    if shape == 'straight':
        return [[x + sign*length*tx, y + sign*length*ty]]
    radius = 2*length/math.pi
    side = 1 if shape == 'arc-left' else -1
    # Chord sagitta at most 0.01 mm; at least nine chords per quarter circle.
    max_step = min(math.pi/18, 2*math.acos(max(-1, 1-.01/radius)))
    steps = math.ceil((math.pi/2)/max_step)
    result = []
    for i in range(1, steps+1):
        theta = (math.pi/2)*i/steps
        along = sign*radius*math.sin(theta)
        across = side*radius*(1-math.cos(theta))
        result.append([x+along*tx-across*ty, y+along*ty+across*tx])
    return list(reversed(result)) if entering else result


def _gap_boundaries(length, offset, points, settings):
    pattern = settings['pattern']
    if pattern == 'continuous':
        return []
    gap = settings['gap']
    if pattern == 'bridges':
        if len(points) < 4 or math.dist(points[0],points[-1]) > 1e-7:
            raise ValueError('Laser holding bridges require closed contours')
        count = settings['count']
        pitch = length/count
        if gap >= pitch - 1e-9:
            raise ValueError('Holding bridges cover the contour; reduce their width or count')
        starts = ((i+.5)*pitch-gap/2 for i in range(count))
    else:
        dash = settings['dash']
        pitch = dash+gap
        count = math.ceil(length/pitch)
        if count > 100000:
            raise ValueError('Perforation exceeds the segment limit; increase the cut or gap length')
        starts = (i*pitch+dash for i in range(count))
    return [offset+v for lo in starts if lo < length for v in (lo,min(length,lo+gap))]


def prepare(path, power, options, width, height, cut=None):
    contour = []
    for point in path:
        if not contour or math.dist(contour[-1], point) > 1e-9:
            contour.append(list(point))
    if len(contour) < 2:
        raise ValueError('Laser paths must contain nonzero travel')
    cut = cut or {'pattern':'continuous', 'shape':'straight'}
    lead_in, lead_out, ramp_in, ramp_out, start, end = options
    before = _lead(*contour[:2], lead_in, cut['shape'], True)
    after = _lead(*contour[-2:], lead_out, cut['shape'], False)
    points = before + contour + after
    if any(x < 0 or x > width or y < 0 or y > height for x, y in points):
        raise ValueError('Laser lead-in/out extends outside the work area; move the object or reduce the lead distance')
    lengths = [math.dist(a, b) for a, b in zip(points, points[1:])]
    total = sum(lengths)
    if ramp_in + ramp_out > total + 1e-9:
        raise ValueError('Power ramp distances overlap; reduce them to fit each path including its leads')
    offset = sum(lengths[:len(before)])
    perimeter = sum(lengths[len(before):len(before)+len(contour)-1])
    gaps = _gap_boundaries(perimeter, offset, contour, cut)
    stops = sorted(set(gaps + [ramp_in, total-ramp_out]))

    def level(distance):
        if bisect_right(gaps, distance) % 2:
            return 0
        if ramp_in and distance < ramp_in:
            return power * (start + (100 - start) * distance / ramp_in) / 100
        if ramp_out and distance > total - ramp_out:
            return power * (end + (100 - end) * (total - distance) / ramp_out) / 100
        return power

    # The S value applies to the full move ending at its associated point.
    output = [(points[0][0], points[0][1], level(0))]
    travelled = 0
    for a, b, length in zip(points, points[1:], lengths):
        if length == 0:
            continue
        stop = travelled + length
        cuts = [travelled, *stops[bisect_right(stops, travelled):bisect_left(stops, stop)], stop]
        for lo, hi in zip(cuts, cuts[1:]):
            changing = (ramp_in and lo < ramp_in) or (ramp_out and hi > total - ramp_out)
            count = max(1, math.ceil((hi - lo) / .5)) if changing else 1
            if len(output) + count > 200000:
                raise ValueError('Laser path generates too many segments')
            for i in range(count):
                s0 = lo + (hi - lo) * i / count
                s1 = lo + (hi - lo) * (i + 1) / count
                t = (s1 - travelled) / length
                output.append((a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, level((s0 + s1) / 2)))
        travelled = stop
    return output
