package au.cncstudio.pocket;

import java.util.ArrayList;

/** Tap selection and default placement for the drawing preview. Plain Java so it can be checked without a device. */
final class Picking {
    /** Distance in millimetres between the default corners of successive new shapes. */
    static final double CASCADE = 5;

    private Picking() {}

    /**
     * Index of the shape a tap at (x, y) selects, or -1 when the tap is clear of every shape.
     * Each box is {x, y, width, height} in millimetres, in drawing order. A shape is a candidate when its box
     * contains the point; when none does, boxes within {@code tolerance} of the point are used instead, so a tap
     * on or just outside an outline still counts. The smallest candidate wins, then the topmost, which reaches a
     * shape nested inside a larger one. Tapping again where the selected shape already is moves to the next
     * candidate, so every shape in a stack can be reached from the drawing.
     */
    static int pick(double[][] boxes, int selected, double x, double y, double tolerance) {
        ArrayList<Integer> hits = within(boxes, x, y, 0);
        if (hits.isEmpty() && tolerance > 0) hits = within(boxes, x, y, tolerance);
        if (hits.isEmpty()) return -1;
        hits.sort((a, b) -> {
            int byArea = Double.compare(boxes[a][2] * boxes[a][3], boxes[b][2] * boxes[b][3]);
            return byArea != 0 ? byArea : Integer.compare(b, a);
        });
        int current = hits.indexOf(selected);
        return hits.get(current < 0 ? 0 : (current + 1) % hits.size());
    }

    private static ArrayList<Integer> within(double[][] boxes, double x, double y, double grow) {
        ArrayList<Integer> hits = new ArrayList<>();
        for (int i = 0; i < boxes.length; i++) {
            double[] b = boxes[i];
            if (x >= b[0] - grow && x <= b[0] + b[2] + grow && y >= b[1] - grow && y <= b[1] + b[3] + grow) hits.add(i);
        }
        return hits;
    }

    /**
     * Default lower-left corner for a new shape of the given size. Starts at (originX, originY) and steps
     * diagonally by {@link #CASCADE} until no existing shape has that corner, so new shapes do not hide one
     * another. Falls back to the origin when the next step would leave the work area.
     */
    static double[] place(double[][] boxes, double originX, double originY, double width, double height,
                          double bedWidth, double bedHeight) {
        for (int step = 0; step <= boxes.length; step++) {
            double x = originX + step * CASCADE, y = originY + step * CASCADE;
            if (x + width > bedWidth + 1e-8 || y + height > bedHeight + 1e-8) break;
            boolean taken = false;
            for (double[] b : boxes) if (Math.abs(b[0] - x) < 1e-6 && Math.abs(b[1] - y) < 1e-6) { taken = true; break; }
            if (!taken) return new double[]{x, y};
        }
        return new double[]{originX, originY};
    }
}
