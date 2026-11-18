"""Extrude closed artwork to a watertight triangle mesh, preserving holes."""
from shapely import constrained_delaunay_triangles
from .operations import region,polygons,value


def extrude(data):
    paths=data.get('paths',[])
    if not isinstance(paths,list) or sum(map(len,paths))>30000:raise ValueError('Extrusion needs at most 30000 contour vertices')
    g=region(paths);height=value(data,'height',3,.01,50)
    triangles=[]
    for poly in polygons(g):
        for triangle in constrained_delaunay_triangles(poly).geoms:
            p=list(triangle.exterior.coords)[:3]
            # Ensure top faces point +Z, bottom -Z.
            area=(p[1][0]-p[0][0])*(p[2][1]-p[0][1])-(p[1][1]-p[0][1])*(p[2][0]-p[0][0])
            if area<0:p.reverse()
            triangles.append([[x,y,height] for x,y in p]);triangles.append([[x,y,0] for x,y in reversed(p)])
        from shapely.geometry.polygon import orient
        poly=orient(poly,sign=1)
        for ring in [poly.exterior,*poly.interiors]:
            p=list(ring.coords)
            for a,b in zip(p,p[1:]):
                triangles.extend([[[*a,0],[*b,0],[*b,height]],[[*a,0],[*b,height],[*a,height]]])
    if len(triangles)>150000:raise ValueError('Extrusion exceeds triangle limit')
    lines=['solid cnc_studio_text']
    for t in triangles:
        lines+=['facet normal 0 0 0','outer loop']+[f'vertex {x:.7f} {y:.7f} {z:.7f}' for x,y,z in t]+['endloop','endfacet']
    lines.append('endsolid cnc_studio_text')
    return {'stl':'\n'.join(lines)+'\n','triangles':len(triangles),'height':height}
