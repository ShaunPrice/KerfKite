package au.cncstudio.pocket;

import android.Manifest;
import android.app.Activity;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.graphics.ImageFormat;
import android.hardware.camera2.CameraAccessException;
import android.hardware.camera2.CameraCaptureSession;
import android.hardware.camera2.CameraCharacteristics;
import android.hardware.camera2.CameraDevice;
import android.hardware.camera2.CameraManager;
import android.hardware.camera2.CaptureRequest;
import android.hardware.camera2.params.StreamConfigurationMap;
import android.media.Image;
import android.media.ImageReader;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Looper;
import android.util.Size;
import android.view.Gravity;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.util.Arrays;
import java.util.HashSet;
import java.util.Set;

/** Shows the rear camera and returns the first Studio pairing link read from a QR code. */
public final class ScanActivity extends Activity implements SurfaceHolder.Callback {
    static final String LINK = "au.cncstudio.pocket.SCANNED_LINK";
    private static final int CAMERA_REQUEST = 1;
    private static final String AIM = "Hold the QR code shown by Studio inside the square.";

    private final Handler main = new Handler(Looper.getMainLooper());
    private SurfaceView preview;
    private TextView status;
    private HandlerThread thread;
    private Handler background;
    private CameraDevice camera;
    private CameraCaptureSession session;
    private ImageReader frames;
    private String cameraId;
    private Size size;
    private boolean upright, surfaceReady, opening, resumed, delivered;
    private byte[] luminance;

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        // The code on screen is the pairing secret: keep it out of screenshots and the recent-apps view.
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_SECURE | WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        LinearLayout page = new LinearLayout(this);
        page.setOrientation(LinearLayout.VERTICAL);
        page.setBackgroundColor(0xff112b2c);
        page.setOnApplyWindowInsetsListener((v, insets) -> {
            v.setPadding(insets.getSystemWindowInsetLeft() + dp(16), insets.getSystemWindowInsetTop() + dp(12),
                    insets.getSystemWindowInsetRight() + dp(16), insets.getSystemWindowInsetBottom() + dp(16));
            return insets;
        });
        TextView title = label("Scan pairing code", 24);
        page.addView(title);
        status = label(AIM, 15);
        status.setAccessibilityLiveRegion(View.ACCESSIBILITY_LIVE_REGION_POLITE);
        status.setPadding(0, dp(6), 0, dp(10));
        page.addView(status);
        chooseCamera();
        preview = new SurfaceView(this);
        preview.setContentDescription("Camera preview");
        FrameLayout frame = new FrameLayout(this) {
            // Keep the picture's proportions: the camera stream is not stretched to the space available.
            @Override protected void onMeasure(int widthSpec, int heightSpec) {
                int width = MeasureSpec.getSize(widthSpec), height = MeasureSpec.getSize(heightSpec);
                if (size != null && width > 0 && height > 0) {
                    // Height over width of the picture as shown.
                    double ratio = upright ? (double) size.getWidth() / size.getHeight()
                            : (double) size.getHeight() / size.getWidth();
                    if (width * ratio > height) width = (int) Math.round(height / ratio);
                    else height = (int) Math.round(width * ratio);
                }
                super.onMeasure(MeasureSpec.makeMeasureSpec(width, MeasureSpec.EXACTLY),
                        MeasureSpec.makeMeasureSpec(height, MeasureSpec.EXACTLY));
            }
        };
        frame.addView(preview, new FrameLayout.LayoutParams(-1, -1));
        View square = new View(this) {
            @Override protected void onDraw(android.graphics.Canvas canvas) {
                android.graphics.Paint pen = new android.graphics.Paint();
                pen.setStyle(android.graphics.Paint.Style.STROKE);
                pen.setStrokeWidth(dp(3));
                pen.setColor(0xff8aebce);
                float side = Math.min(getWidth(), getHeight()) - dp(6);
                float left = (getWidth() - side) / 2f, top = (getHeight() - side) / 2f;
                canvas.drawRect(left, top, left + side, top + side, pen);
            }
        };
        square.setWillNotDraw(false);
        frame.addView(square, new FrameLayout.LayoutParams(-1, -1));
        LinearLayout.LayoutParams fill = new LinearLayout.LayoutParams(-1, 0, 1);
        fill.gravity = Gravity.CENTER_HORIZONTAL;
        page.addView(frame, fill);
        Button cancel = new Button(this);
        cancel.setText("Cancel");
        cancel.setAllCaps(false);
        cancel.setMinHeight(dp(48));
        cancel.setOnClickListener(v -> finish());
        LinearLayout.LayoutParams below = new LinearLayout.LayoutParams(-1, -2);
        below.topMargin = dp(12);
        page.addView(cancel, below);
        setContentView(page);
        if (size != null) preview.getHolder().setFixedSize(size.getWidth(), size.getHeight());
        preview.getHolder().addCallback(this);
        if (cameraId == null) status.setText("This phone has no rear camera. Paste the pairing link instead.");
        else if (!permitted()) requestPermissions(new String[]{Manifest.permission.CAMERA}, CAMERA_REQUEST);
    }

    private int dp(int value) { return Math.round(getResources().getDisplayMetrics().density * value); }

    private TextView label(String text, int sp) {
        TextView view = new TextView(this);
        view.setText(text);
        view.setTextSize(sp);
        view.setTextColor(0xfff5f7f5);
        return view;
    }

    private boolean permitted() {
        return checkSelfPermission(Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED;
    }

    /** The rear camera and a stream size both the preview and the frame reader support, near 1280 × 720. */
    private void chooseCamera() {
        CameraManager manager = getSystemService(CameraManager.class);
        try {
            for (String id : manager.getCameraIdList()) {
                CameraCharacteristics about = manager.getCameraCharacteristics(id);
                Integer facing = about.get(CameraCharacteristics.LENS_FACING);
                StreamConfigurationMap streams = about.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP);
                if (facing == null || facing != CameraCharacteristics.LENS_FACING_BACK || streams == null) continue;
                Size[] readable = streams.getOutputSizes(ImageFormat.YUV_420_888);
                Size[] viewable = streams.getOutputSizes(SurfaceHolder.class);
                if (readable == null || viewable == null) continue;
                Set<Size> shown = new HashSet<>(Arrays.asList(viewable));
                Size best = null;
                long target = 1280L * 720;
                for (Size candidate : readable) {
                    if (!shown.contains(candidate) || candidate.getWidth() > 1920 || candidate.getHeight() > 1080) continue;
                    long area = (long) candidate.getWidth() * candidate.getHeight();
                    if (best == null || Math.abs(area - target) < Math.abs((long) best.getWidth() * best.getHeight() - target))
                        best = candidate;
                }
                if (best == null) continue;
                Integer mounted = about.get(CameraCharacteristics.SENSOR_ORIENTATION);
                int turned = new int[]{0, 90, 180, 270}[getWindowManager().getDefaultDisplay().getRotation()];
                // The stream is landscape on the sensor. When the sensor and the display differ by a quarter turn,
                // as on a phone held upright, the picture is shown taller than it is wide.
                upright = ((mounted == null ? 90 : mounted) - turned + 360) % 180 != 0;
                cameraId = id;
                size = best;
                return;
            }
        } catch (CameraAccessException | RuntimeException unavailable) {
            cameraId = null;
        }
    }

    @Override public void onRequestPermissionsResult(int request, String[] permissions, int[] results) {
        // An empty result means the question was interrupted, not answered: the permission itself is checked below.
        if (request != CAMERA_REQUEST) return;
        if (permitted()) open();
        else if (results.length > 0)
            status.setText("Camera access was not allowed. Allow it in Settings, or paste the pairing link instead.");
    }

    @Override protected void onResume() {
        super.onResume();
        resumed = true;
        thread = new HandlerThread("pairing-scan");
        thread.start();
        background = new Handler(thread.getLooper());
        open();
    }

    @Override protected void onPause() {
        resumed = false;
        close();
        thread.quitSafely();
        thread = null;
        background = null;
        super.onPause();
    }

    @Override public void surfaceCreated(SurfaceHolder holder) {}

    @Override public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        surfaceReady = size != null && width == size.getWidth() && height == size.getHeight();
        open();
    }

    @Override public void surfaceDestroyed(SurfaceHolder holder) {
        surfaceReady = false;
        close();
    }

    private void open() {
        if (!resumed || !surfaceReady || opening || camera != null || cameraId == null || !permitted()) return;
        opening = true;
        final Handler worker = background;
        try {
            getSystemService(CameraManager.class).openCamera(cameraId, new CameraDevice.StateCallback() {
                @Override public void onOpened(CameraDevice device) {
                    main.post(() -> {
                        opening = false;
                        if (!resumed || !surfaceReady || worker != background) { device.close(); return; }
                        camera = device;
                        start(worker);
                    });
                }
                @Override public void onDisconnected(CameraDevice device) { device.close(); main.post(() -> lost(device)); }
                @Override public void onError(CameraDevice device, int error) {
                    device.close();
                    main.post(() -> { lost(device); status.setText("The camera could not be started. Paste the pairing link instead."); });
                }
            }, worker);
        } catch (CameraAccessException | SecurityException | IllegalArgumentException failed) {
            opening = false;
            status.setText("The camera could not be started. Paste the pairing link instead.");
        }
    }

    private void lost(CameraDevice device) {
        opening = false;
        if (camera == device) { camera = null; session = null; }
    }

    private void start(Handler worker) {
        final int width = size.getWidth(), height = size.getHeight();
        frames = ImageReader.newInstance(width, height, ImageFormat.YUV_420_888, 2);
        frames.setOnImageAvailableListener(reader -> {
            String text;
            try (Image image = reader.acquireLatestImage()) {
                if (image == null) return;
                Image.Plane plane = image.getPlanes()[0];
                luminance = QrFrames.pack(plane.getBuffer(), plane.getRowStride(), width, height, luminance);
            } catch (RuntimeException closed) {
                return;
            }
            text = QrFrames.decode(luminance, width, height);
            if (text != null) main.post(() -> read(text));
        }, worker);
        try {
            final CameraDevice device = camera;
            final Surface shown = preview.getHolder().getSurface(), read = frames.getSurface();
            device.createCaptureSession(Arrays.asList(shown, read), new CameraCaptureSession.StateCallback() {
                @Override public void onConfigured(CameraCaptureSession configured) {
                    try {
                        CaptureRequest.Builder request = device.createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW);
                        request.addTarget(shown);
                        request.addTarget(read);
                        request.set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_PICTURE);
                        configured.setRepeatingRequest(request.build(), null, worker);
                        main.post(() -> { if (camera == device) { session = configured; status.setText(AIM); } });
                    } catch (CameraAccessException | IllegalStateException | IllegalArgumentException stopped) {
                        // The camera was closed while it was being set up.
                    }
                }
                @Override public void onConfigureFailed(CameraCaptureSession failed) {
                    main.post(() -> status.setText("The camera could not be started. Paste the pairing link instead."));
                }
            }, worker);
        } catch (CameraAccessException | IllegalStateException | IllegalArgumentException failed) {
            status.setText("The camera could not be started. Paste the pairing link instead.");
        }
    }

    /** A code was read. Only a Studio pairing link ends the scan; anything else is named and scanning continues. */
    private void read(String text) {
        if (delivered || !resumed) return;
        try {
            new PendantClient(text);
        } catch (IllegalArgumentException other) {
            status.setText("That code is not a Studio pairing link. " + AIM);
            return;
        }
        delivered = true;
        setResult(RESULT_OK, new Intent().putExtra(LINK, text));
        finish();
    }

    private void close() {
        opening = false;
        if (session != null) { session.close(); session = null; }
        if (camera != null) { camera.close(); camera = null; }
        if (frames != null) { frames.close(); frames = null; }
    }
}
