package au.cncstudio.pocket;

import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.util.ArrayList;
import java.util.List;
import java.util.Random;

/**
 * Decodes the symbols Studio draws for pairing links from simulated camera frames, with the decoder Pocket uses.
 * Run by validation/pairing_qr_check.py, which writes the symbols with Studio's own encoder.
 * Argument: a text file of blocks separated by blank lines, each a link followed by rows of 0 and 1.
 */
public final class QrFramesCheck {
    private static final int WIDTH = 1280, HEIGHT = 720;
    private static int passed;

    private static void check(boolean condition, String what) {
        if (!condition) throw new AssertionError(what);
        passed++;
    }

    /** A frame with the symbol drawn at {@code scale} pixels a module, its centre moved by (dx, dy) from the middle. */
    private static byte[] frame(boolean[][] rows, int scale, int dx, int dy, boolean quarterTurn, long seed) {
        Random noise = new Random(seed);
        byte[] out = new byte[WIDTH * HEIGHT];
        for (int i = 0; i < out.length; i++) out[i] = (byte) (70 + noise.nextInt(25));  // a dim room around the screen
        int n = rows.length, quiet = 4, side = (n + 2 * quiet) * scale;
        int left = (WIDTH - side) / 2 + dx, top = (HEIGHT - side) / 2 + dy;
        for (int y = 0; y < side; y++) {
            for (int x = 0; x < side; x++) {
                int px = left + x, py = top + y;
                if (px < 0 || py < 0 || px >= WIDTH || py >= HEIGHT) continue;
                int mx = x / scale - quiet, my = y / scale - quiet;
                if (quarterTurn) { int swap = mx; mx = my; my = n - 1 - swap; }
                boolean dark = mx >= 0 && my >= 0 && mx < n && my < n && rows[my][mx];
                out[py * WIDTH + px] = (byte) ((dark ? 38 : 205) + noise.nextInt(25) - 12);
            }
        }
        return out;
    }

    /** The frame as a camera hands it over: each row followed by padding, except the last. */
    private static ByteBuffer padded(byte[] frame, int rowStride) {
        ByteBuffer plane = ByteBuffer.allocate(rowStride * (HEIGHT - 1) + WIDTH);
        for (int y = 0; y < HEIGHT; y++) {
            plane.position(y * rowStride);
            plane.put(frame, y * WIDTH, WIDTH);
            for (int x = WIDTH; x < rowStride && y < HEIGHT - 1; x++) plane.put((byte) 0xff);
        }
        plane.rewind();
        return plane;
    }

    public static void main(String[] args) throws Exception {
        List<String> lines = Files.readAllLines(Paths.get(args[0]), StandardCharsets.UTF_8);
        lines.add("");
        String link = null;
        List<boolean[]> rows = new ArrayList<>();
        int symbols = 0;
        byte[] reuse = null;
        for (String line : lines) {
            if (!line.isEmpty()) {
                if (link == null) { link = line; continue; }
                boolean[] row = new boolean[line.length()];
                for (int i = 0; i < row.length; i++) row[i] = line.charAt(i) == '1';
                rows.add(row);
                continue;
            }
            if (link == null) continue;
            boolean[][] symbol = rows.toArray(new boolean[0][]);
            int largest = (HEIGHT - 8) / (symbol.length + 8);
            check(largest >= 4, "The symbol is too dense for a 720-line frame: " + symbol.length + " modules.");
            for (int scale : new int[]{3, 4, largest}) {
                for (int stride : new int[]{WIDTH, WIDTH + 32}) {
                    byte[] plain = frame(symbol, scale, 0, 0, false, 11L * scale + stride);
                    reuse = QrFrames.pack(padded(plain, stride), stride, WIDTH, HEIGHT, reuse);
                    check(java.util.Arrays.equals(reuse, plain), "Row padding was not removed.");
                    check(link.equals(QrFrames.decode(reuse, WIDTH, HEIGHT)), "Not read at " + scale + " px a module.");
                }
            }
            check(link.equals(QrFrames.decode(frame(symbol, 4, 0, 0, true, 5), WIDTH, HEIGHT)), "Not read after a quarter turn.");
            int room = (HEIGHT - (symbol.length + 8) * 4) / 2;
            check(link.equals(QrFrames.decode(frame(symbol, 4, room, -room, false, 6), WIDTH, HEIGHT)), "Not read off-centre in the square.");
            check(QrFrames.decode(frame(symbol, 4, -(WIDTH / 2), 0, false, 7), WIDTH, HEIGHT) == null, "A code outside the square was read.");
            symbols++;
            link = null;
            rows.clear();
        }
        check(symbols >= 2, "Expected a pinned and a plain pairing link.");
        check(QrFrames.decode(new byte[WIDTH * HEIGHT], WIDTH, HEIGHT) == null, "An empty frame produced text.");
        System.out.println("PASS " + passed + " pairing QR checks across " + symbols + " symbols");
    }
}
