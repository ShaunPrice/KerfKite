package au.cncstudio.pocket;

import java.util.Locale;

final class Part {
    static final String[] TYPES = {"Rectangle", "Ellipse", "Polygon"};
    final int type, sides;
    final double x, y, width, height;

    Part(int type, int sides, double x, double y, double width, double height) {
        if (type < 0 || type >= TYPES.length || sides < 3 || sides > 64)
            throw new IllegalArgumentException("Use 3–64 polygon sides.");
        if (!(width >= 0.2 && height >= 0.2 && x >= 0 && y >= 0 && x + width <= 680 && y + height <= 240))
            throw new IllegalArgumentException("Fit the shape inside the 680 × 240 mm work area.");
        this.type = type; this.sides = sides; this.x = x; this.y = y; this.width = width; this.height = height;
    }

    double[][] outline() {
        if (type == 0) return new double[][]{{x,y},{x+width,y},{x+width,y+height},{x,y+height},{x,y}};
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
        return path;
    }

    String title() {
        return type == 2 ? String.format(Locale.ROOT, "%d-sided polygon", sides) : TYPES[type];
    }
}
