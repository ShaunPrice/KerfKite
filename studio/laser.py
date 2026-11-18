"""Straight tangent leads and distance-based, sampled laser power ramps."""
import math


def prepare(path, power, options, width, height):
    points = []
    for point in path:
        if not points or math.dist(points[-1], point) > 1e-9:
            points.append(list(point))
    if len(points) < 2:
        raise ValueError('Laser paths must contain nonzero travel')
    lead_in, lead_out, ramp_in, ramp_out, start, end = options
    if lead_in:
        a, b = points[:2]
        length = math.dist(a, b)
        points.insert(0, [a[i] - (b[i] - a[i]) / length * lead_in for i in (0, 1)])
    if lead_out:
        a, b = points[-2:]
        length = math.dist(a, b)
        points.append([b[i] + (b[i] - a[i]) / length * lead_out for i in (0, 1)])
    if any(x < 0 or x > width or y < 0 or y > height for x, y in points):
        raise ValueError('Laser lead-in/out extends outside the work area; move the object or reduce the lead distance')
    lengths = [math.dist(a, b) for a, b in zip(points, points[1:])]
    total = sum(lengths)
    if ramp_in + ramp_out > total + 1e-9:
        raise ValueError('Power ramp distances overlap; reduce them to fit each path including its leads')

    def level(distance):
        if ramp_in and distance < ramp_in:
            return power * (start + (100 - start) * distance / ramp_in) / 100
        if ramp_out and distance > total - ramp_out:
            return power * (end + (100 - end) * (total - distance) / ramp_out) / 100
        return power

    # Tuple endpoints: x, y, power during the segment ending here. Segments crossing
    # ramp boundaries are split exactly. Within ramps sample at <=0.5 mm intervals.
    output = [(points[0][0], points[0][1], level(0))]
    travelled = 0
    for a, b, length in zip(points, points[1:], lengths):
        stop = travelled + length
        cuts = sorted(set([travelled, stop] + [v for v in (ramp_in, total - ramp_out) if travelled < v < stop]))
        for lo, hi in zip(cuts, cuts[1:]):
            changing = (ramp_in and lo < ramp_in) or (ramp_out and hi > total - ramp_out)
            count = max(1, math.ceil((hi - lo) / .5)) if changing else 1
            if len(output) + count > 200000:
                raise ValueError('Laser ramp generates too many segments')
            for i in range(count):
                s0 = lo + (hi - lo) * i / count
                s1 = lo + (hi - lo) * (i + 1) / count
                t = (s1 - travelled) / length
                output.append((a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, level((s0 + s1) / 2)))
        travelled = stop
    return output
