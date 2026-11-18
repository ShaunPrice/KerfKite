"""Explicit router drilling moves for point markers and circular hole outlines."""
import math
from .protocols import get_protocol, postprocess


def hole_center(path, diameter):
    points = [path[0]]
    for point in path[1:]:
        if math.dist(points[-1],point)>1e-8:
            points.append(point)
    a = points[0]
    if len(points) == 1:
        return a
    if len(points)<9 or math.dist(a,points[-1])>1e-6:
        raise ValueError('Drill requires point markers or closed circular hole outlines')
    ring = points[:-1]
    b = max(ring, key=lambda p:math.dist(a,p))
    u, v = b[0]-a[0], b[1]-a[1]
    c = max(ring, key=lambda p:abs(u*(p[1]-a[1])-v*(p[0]-a[0])))
    m, n = c[0]-a[0], c[1]-a[1]
    det = 2*(u*n-v*m)
    if abs(det)<1e-12:
        raise ValueError('Drill outline is degenerate')
    ab, ac = u*u+v*v, m*m+n*n
    center = [a[0]+(n*ab-v*ac)/det, a[1]+(u*ac-m*ab)/det]
    radius = diameter/2
    if any(abs(math.dist(p,center)-radius)>max(.0001,.002*radius) for p in ring):
        raise ValueError('Drill outline must be circular and match the cutter diameter; use a profile or pocket for a larger hole')
    # A full turn with regular coverage distinguishes a bore from a star or partial arc.
    directions = [math.atan2(p[1]-center[1],p[0]-center[0]) for p in ring]
    angles = [math.remainder(q-p,2*math.pi) for p,q in zip(directions,directions[1:]+directions[:1])]
    if max(map(abs,angles))>math.pi/4+1e-5 or abs(abs(sum(angles))-2*math.pi)>1e-5 or sum(map(abs,angles))>2*math.pi+1e-5:
        raise ValueError('Drill outline must trace one complete circle without reversing')
    return center


def generate_drill(project, paths, number):
    if project['mode']!='router' or project.get('path_depths') is not None or project.get('path_powers') is not None:
        raise ValueError('Drilling requires a router and a single target depth')
    diameter = number(project.get('tool_diameter',3.175),'Drill diameter',.01,100)
    depth = number(project.get('depth',1),'Drill depth',.01,50)
    safe = number(project.get('safe',5),'Clearance Z',.1,100)
    plunge = number(project.get('plunge',100),'Drill plunge feed',1,1000)
    power = number(project.get('power'),'Spindle RPM',1,100000)
    peck = number(project.get('drill_peck',0),'Peck depth',0,50)
    if peck and peck<.01:
        raise ValueError('Use zero for one plunge or a peck depth of at least 0.01 mm')
    angle = number(project.get('drill_point_angle',118),'Drill point angle',60,170)
    shape = project.get('tool_shape','drill')
    if shape not in ('drill','flat'):
        raise ValueError('Drilling requires a drill bit or centre-cutting flat end mill')
    count = max(1,math.ceil(depth/peck-1e-10)) if peck else 1
    if len(paths)*(2*count+1)+10>200000:
        raise ValueError('Drilling exceeds the line limit; increase the peck depth or reduce hole count')
    centers = [hole_center(path,diameter) for path in paths]
    radius = diameter/2
    width, height = float(project['width']),float(project['height'])
    if any(x-radius < -1e-8 or y-radius < -1e-8 or x+radius > width+1e-8 or y+radius > height+1e-8 for x,y in centers):
        raise ValueError('Drill footprint crosses the work area boundary')
    lines=['G21','G90','G17','G94','G54','M5',f'G0 Z{safe:.3f}',f'M3 S{power:.0f}','G4 P2']
    segments=[]
    length=0
    for x,y in centers:
        lines.append(f'G0 X{x:.3f} Y{y:.3f}')
        for i in range(count):
            z = -depth if i+1 == count else -(i+1)*peck
            lines.extend((f'G1 Z{z:.3f} F{plunge:.1f}',f'G0 Z{safe:.3f}'))
            segments.append([x,y,x,y,z,None,safe])
            length += safe-z
    lines.append('M5')
    protocol=project.get('protocol','grbl')
    return dict(lines=postprocess(lines,protocol,'router',project.get('dwell_units','')), segments=segments,
                cut_length=round(length,2),cut_minutes=round(length/plunge,2),mode='router',
                bounds=[min(p[0] for p in centers),min(p[1] for p in centers),max(p[0] for p in centers),max(p[1] for p in centers)],
                warnings=[get_protocol(protocol).notes,'Depth is measured to the tool tip. Drilling time includes feed plunges from clearance; rapid retracts and spindle startup are excluded.'],
                drill_holes=[[x,y,diameter] for x,y in centers],
                simulation_tool=dict(diameter=diameter,shape=shape,angle=angle), prepared_paths=paths,prepared_depths=None)
