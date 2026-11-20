package au.cncstudio.pocket;

/**
 * Dependency-free check of {@link Picking}. From the android directory:
 *
 *   javac -d build/picking-check app/src/main/java/au/cncstudio/pocket/Picking.java app/src/test/java/au/cncstudio/pocket/PickingCheck.java
 *   java -cp build/picking-check au.cncstudio.pocket.PickingCheck
 */
public final class PickingCheck {
    private static int checks;

    private static void same(int expected, int actual, String what) {
        checks++;
        if (expected != actual) throw new AssertionError(what + ": expected " + expected + " but was " + actual);
    }

    private static void corner(double x, double y, double[] actual, String what) {
        checks++;
        if (Math.abs(actual[0] - x) > 1e-9 || Math.abs(actual[1] - y) > 1e-9)
            throw new AssertionError(what + ": expected (" + x + ", " + y + ") but was (" + actual[0] + ", " + actual[1] + ")");
    }

    public static void main(String[] args) {
        // Two shapes left at the old shared default: the case recorded on a Galaxy S23 Ultra.
        double[][] stacked = {{10, 10, 40, 25}, {10, 10, 40, 25}};
        same(1, Picking.pick(stacked, -1, 30, 22, 0), "nothing selected picks the topmost of a stack");
        same(0, Picking.pick(stacked, 1, 30, 22, 0), "a second tap reaches the shape underneath");
        same(1, Picking.pick(stacked, 0, 30, 22, 0), "a third tap returns to the top");
        same(-1, Picking.pick(stacked, 1, 80, 80, 0), "a tap clear of every shape selects nothing");

        // Taps a little outside the outline, as a pen or fingertip lands in practice.
        same(-1, Picking.pick(stacked, -1, 9.5, 9.5, 0), "no tolerance misses just outside the corner");
        same(1, Picking.pick(stacked, -1, 9.5, 9.5, 1), "tolerance accepts a tap just outside the corner");
        same(-1, Picking.pick(stacked, -1, 8, 8, 1), "tolerance is bounded");

        // A precise hit is preferred to a neighbour that is only within tolerance.
        double[][] neighbours = {{0, 0, 10, 10}, {10.5, 0, 10, 10}};
        same(0, Picking.pick(neighbours, -1, 9.9, 5, 1), "the shape containing the point beats a near neighbour");
        same(0, Picking.pick(neighbours, 0, 9.9, 5, 1), "a near neighbour is not cycled to while the point is inside a shape");
        same(1, Picking.pick(neighbours, -1, 10.3, 5, 1), "between two shapes the later one wins at equal size");

        // A small shape drawn before a large one that covers it.
        double[][] nested = {{20, 20, 5, 5}, {0, 0, 100, 60}};
        same(0, Picking.pick(nested, -1, 22, 22, 0), "the smaller shape wins inside a larger one");
        same(1, Picking.pick(nested, 0, 22, 22, 0), "tapping again moves out to the larger shape");
        same(1, Picking.pick(nested, -1, 60, 40, 0), "outside the small shape only the large one is hit");
        same(1, Picking.pick(nested, 1, 60, 40, 0), "a single candidate stays selected");
        same(-1, Picking.pick(new double[0][], -1, 1, 1, 1), "an empty drawing selects nothing");

        // Default placement on the 680 x 240 mm work area.
        corner(10, 10, Picking.place(new double[0][], 10, 10, 40, 25, 680, 240), "the first shape keeps the documented default");
        corner(15, 15, Picking.place(new double[][]{{10, 10, 40, 25}}, 10, 10, 40, 25, 680, 240), "the second shape steps clear of the first");
        corner(20, 20, Picking.place(new double[][]{{10, 10, 40, 25}, {15, 15, 40, 25}}, 10, 10, 40, 25, 680, 240), "the third shape steps again");
        corner(10, 10, Picking.place(new double[][]{{15, 15, 40, 25}}, 10, 10, 40, 25, 680, 240), "a freed default corner is reused");
        corner(0, 0, Picking.place(new double[][]{{0, 0, 30, 30}}, 0, 0, 30, 30, 30, 30), "a full work area falls back to the origin");
        corner(10, 10, Picking.place(new double[][]{{10, 10, 40, 25}}, 10, 10, 40, 25, 52, 36), "no room to step falls back to the origin");

        System.out.println("PASS " + checks + " picking and placement checks");
    }
}
