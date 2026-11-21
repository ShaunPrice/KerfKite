package au.cncstudio.pocket;

import com.google.zxing.BinaryBitmap;
import com.google.zxing.DecodeHintType;
import com.google.zxing.PlanarYUVLuminanceSource;
import com.google.zxing.common.HybridBinarizer;
import com.google.zxing.qrcode.QRCodeReader;

import java.nio.ByteBuffer;
import java.util.EnumMap;
import java.util.Map;

/** Reads a QR code from a camera frame's luminance plane. Plain Java so it can be checked without a device. */
final class QrFrames {
    private static final Map<DecodeHintType, Object> HINTS = new EnumMap<>(DecodeHintType.class);
    static {
        HINTS.put(DecodeHintType.TRY_HARDER, Boolean.TRUE);
        HINTS.put(DecodeHintType.CHARACTER_SET, "UTF-8");
    }

    private QrFrames() {}

    /**
     * The luminance plane as width × height bytes, without the padding a camera may add to each row.
     * {@code reuse} is filled and returned when it has the right length, which avoids an allocation per frame.
     */
    static byte[] pack(ByteBuffer plane, int rowStride, int width, int height, byte[] reuse) {
        byte[] out = reuse != null && reuse.length == width * height ? reuse : new byte[width * height];
        ByteBuffer rows = plane.duplicate();
        for (int y = 0; y < height; y++) {
            rows.position(y * rowStride);
            rows.get(out, y * width, width);
        }
        return out;
    }

    /**
     * The text of a QR code in the central square of the frame, or null when none can be read there.
     * The square is where the scanner asks for the code to be held, and it halves the work on a wide frame.
     */
    static String decode(byte[] luminance, int width, int height) {
        int side = Math.min(width, height);
        PlanarYUVLuminanceSource source = new PlanarYUVLuminanceSource(
                luminance, width, height, (width - side) / 2, (height - side) / 2, side, side, false);
        try {
            return new QRCodeReader().decode(new BinaryBitmap(new HybridBinarizer(source)), HINTS).getText();
        } catch (Exception unreadable) {
            // No code, a damaged code, or a frame the reader could not follow: the next frame is tried instead.
            return null;
        }
    }
}
