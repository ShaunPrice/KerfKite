package au.cncstudio.pocket;

import org.json.JSONArray;
import org.json.JSONException;
import java.util.Locale;

final class Part {
    static final String[] TYPES = {"Rectangle", "Ellipse", "Polygon"};
    static final int PATH = 3;
    final int type, sides;
    final double x, y, width, height;
    final String name;
    private final double[][][] geometry;
    private double[][][] outline; // A shape never changes, so its outline is computed once. Callers must not modify it.

    Part(int type, int sides, double x, double y, double width, double height) {
        if (type < 0 || type >= TYPES.length || sides < 3 || sides > 64)
            throw new IllegalArgumentException("Use 3–64 polygon sides.");
        if (!(Double.isFinite(x) && Double.isFinite(y) && Double.isFinite(width) && Double.isFinite(height)
                && width >= .2 && height >= .2))
            throw new IllegalArgumentException("Use finite dimensions of at least 0.2 mm.");
        this.type=type; this.sides=sides; this.x=x; this.y=y; this.width=width; this.height=height;
        this.name=type==2 ? String.format(Locale.ROOT,"%d-sided polygon",sides) : TYPES[type];
        geometry=null;
    }

    private Part(String name, double[][][] paths) {
        double left=Double.POSITIVE_INFINITY,bottom=left,right=Double.NEGATIVE_INFINITY,top=right;
        for(double[][] path:paths) for(double[] pt:path) {
            left=Math.min(left,pt[0]);right=Math.max(right,pt[0]);bottom=Math.min(bottom,pt[1]);top=Math.max(top,pt[1]);
        }
        this.name=name;this.type=PATH;this.sides=0;this.x=left;this.y=bottom;
        this.width=right-left;this.height=top-bottom;this.geometry=paths;
    }

    void checkBed(double bedWidth,double bedHeight) {
        if(x < -1e-8 || y < -1e-8 || x+width > bedWidth+1e-8 || y+height > bedHeight+1e-8)
            throw new IllegalArgumentException("Fit the artwork inside the project work area.");
    }

    static Part fromPaths(String name, JSONArray array, int[] points) throws JSONException {
        if(array.length()==0)throw new JSONException("An object has no vector paths.");
        double[][][] paths=new double[array.length()][][];
        for(int i=0;i<paths.length;i++){
            JSONArray line=array.getJSONArray(i);
            points[0]+=line.length();
            if(line.length()<2 || points[0]>100000)throw new JSONException("Use paths of at least two points and up to 100,000 points per project.");
            paths[i]=new double[line.length()][2];
            for(int j=0;j<line.length();j++){
                JSONArray pair=line.getJSONArray(j);
                if(pair.length()!=2)throw new JSONException("Only planar XY artwork can open in Pocket.");
                for(int axis=0;axis<2;axis++){
                    Object value=pair.get(axis);
                    if(!(value instanceof Number) || !Double.isFinite(((Number)value).doubleValue()))
                        throw new JSONException("A path contains an invalid coordinate.");
                    paths[i][j][axis]=((Number)value).doubleValue();
                }
            }
        }
        return new Part(name,paths);
    }

    boolean sameOutline(Part other) {
        double[][][] a=paths(), b=other.paths();
        if(a.length!=b.length)return false;
        for(int i=0;i<a.length;i++) {
            if(a[i].length!=b[i].length)return false;
            for(int j=0;j<a[i].length;j++)for(int k=0;k<2;k++)
                if(Math.abs(a[i][j][k]-b[i][j][k])>1e-9)return false;
        }
        return true;
    }

    double[][][] paths() {
        if(geometry!=null)return geometry;
        if(outline==null)outline=primitivePaths();
        return outline;
    }

    private double[][][] primitivePaths() {
        if(type==0)return new double[][][]{{{x,y},{x+width,y},{x+width,y+height},{x,y+height},{x,y}}};
        int n = type == 1 ? 96 : sides;
        double[][] path = new double[n+1][2];
        double left = 1, right = -1, top = -1, bottom = 1;
        for (int i=0; i<n; i++) {
            double angle = 2 * Math.PI * i / n + Math.PI / 2;
            path[i][0] = Math.cos(angle); path[i][1] = Math.sin(angle);
            left = Math.min(left,path[i][0]); right = Math.max(right,path[i][0]);
            top = Math.max(top,path[i][1]); bottom = Math.min(bottom,path[i][1]);
        }
        for (int i=0; i<n; i++) {
            path[i][0] = x + (path[i][0]-left) * width / (right-left);
            path[i][1] = y + (path[i][1]-bottom) * height / (top-bottom);
        }
        path[n] = path[0].clone();
        return new double[][][]{path};
    }

    Part reposition(double nx,double ny,double w,double h) {
        if(!Double.isFinite(nx)||!Double.isFinite(ny)||!Double.isFinite(w)||!Double.isFinite(h)
                || (width==0 ? w!=0 : w<=0) || (height==0 ? h!=0 : h<=0))
            throw new IllegalArgumentException("Keep zero-width or zero-height axes unchanged and other dimensions positive.");
        double[][][] transformed=new double[geometry.length][][];
        for(int i=0;i<geometry.length;i++){
            transformed[i]=new double[geometry[i].length][2];
            for(int j=0;j<geometry[i].length;j++){
                double[] pt=geometry[i][j];
                transformed[i][j][0]=nx+(width==0?0:(pt[0]-x)/width*w);
                transformed[i][j][1]=ny+(height==0?0:(pt[1]-y)/height*h);
            }
        }
        return new Part(name,transformed);
    }

    JSONArray jsonPaths() throws JSONException {
        JSONArray result=new JSONArray();
        for(double[][] path:paths()){
            JSONArray line=new JSONArray();
            for(double[] pt:path)line.put(new JSONArray().put(pt[0]).put(pt[1]));
            result.put(line);
        }
        return result;
    }
    JSONArray primitive() throws JSONException {
        return new JSONArray().put(type).put(sides).put(x).put(y).put(width).put(height);
    }
    static Part primitive(JSONArray p) throws JSONException {
        return new Part(p.getInt(0),p.getInt(1),p.getDouble(2),p.getDouble(3),p.getDouble(4),p.getDouble(5));
    }
    String title(){return name;}
}
