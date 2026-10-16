package dev.melee;

import android.hardware.camera2.CameraCaptureSession;
import android.hardware.camera2.CameraCharacteristics;
import android.hardware.camera2.CameraDevice;
import android.hardware.camera2.CameraManager;
import android.hardware.camera2.CaptureRequest;
import android.hardware.camera2.CaptureResult;
import android.hardware.camera2.TotalCaptureResult;
import android.util.Log;

/* The room's light, for matching the 3D view to it in mixed reality
 * (docs/xr-auto-room-light.md; extern/aurora/lib/xr, "room light"). Quest
 * builds only: MeleeXrActivity runs one in the immersive game's process.
 *
 * Twice a second it asks the game whether a measurement is due
 * (aurora_xr_room_light_poll: at each placing hold and about every 15 s while
 * the room shows). Then, with the passthrough camera permission
 * (horizonos.permission.HEADSET_CAMERA, asked for in the launcher panel), it
 * opens one passthrough camera (the left one, by Meta's vendor tags) at its
 * smallest YUV size, takes 4 frames, drops the first, and closes it again:
 * the camera runs well under a second every 15 s. From each frame, the mean
 * linear RGB and luminance of the part where the stage usually sits (rows 40
 * to 90%, columns 20 to 80%, every 8th pixel), plus the frame's exposure,
 * sensitivity and white balance gains where the camera reports them. The
 * camera shares passthrough's auto exposure, so this is the brightness the
 * player sees, not the room's lux. Without the camera (Quest 2 and Pro, the
 * permission denied, repeated failures) it falls back to the light sensor,
 * if the system exposes one; without either, the game counts the room as
 * normal. Everything runs on its own thread; the game never waits on it,
 * and frames never leave it.
 *
 * AURORA_XR_ROOM_LIGHT_LOG=1 logs every camera's characteristics and every
 * sensor once at start, and each burst (tag MeleeLight);
 * AURORA_XR_ROOM_LIGHT_SOURCE=camera or sensor uses only that one. */
final class RoomLight {
    private static final String TAG = "MeleeLight";
    private static final int SOURCE_CAMERA = 1;
    private static final int SOURCE_SENSOR = 2;
    private static final long FIRST_POLL_MS = 3000; /* the game reads its env file first */
    private static final long POLL_MS = 500;
    private static final int BURST_FRAMES = 4; /* the first one dropped: exposure may still settle */
    private static final long BURST_TIMEOUT_MS = 4000;
    private static final int MAX_FAILURES = 3; /* bursts in a row, then the sensor */

    private static native int nativePoll();

    private static native void nativeSample(int source, float luma, float r, float g, float b, float exposureMs,
            float iso, float wbR, float wbGEven, float wbGOdd, float wbB, float lux);

    private final android.content.Context mContext;
    private android.os.HandlerThread mThread;
    private android.os.Handler mHandler;
    private boolean mSetUp;
    private boolean mLog;
    private String mCameraId; /* null: no camera to use */
    private android.util.Size mSize;
    private android.util.Range<Integer> mFps;
    private int mFailures;
    /* A burst in progress (the handler's thread only). */
    private CameraDevice mDevice;
    private CameraCaptureSession mSession;
    private android.media.ImageReader mReader;
    private volatile CaptureResult mResult;
    private int mFrames;
    private int mCount;
    private double mSumR, mSumG, mSumB, mSumL;
    private long mBurstStart;
    /* The light sensor, without a camera. */
    private android.hardware.SensorManager mSensors;
    private android.hardware.SensorEventListener mListener;
    private volatile float mLux = -1f;

    private final Runnable mPoll = this::poll;
    private final Runnable mTimeout = () -> burstFailed("timed out");

    RoomLight(android.content.Context context) {
        mContext = context.getApplicationContext();
    }

    void start() {
        if (mThread != null || android.os.Build.VERSION.SDK_INT < android.os.Build.VERSION_CODES.P) {
            return;
        }
        mThread = new android.os.HandlerThread("melee-room-light");
        mThread.start();
        mHandler = new android.os.Handler(mThread.getLooper());
        mHandler.postDelayed(mPoll, FIRST_POLL_MS);
    }

    void stop() {
        if (mThread == null) {
            return;
        }
        final android.os.HandlerThread thread = mThread;
        mHandler.removeCallbacksAndMessages(null);
        mHandler.post(() -> {
            closeCamera();
            stopSensor();
            thread.quitSafely();
        });
        mThread = null;
    }

    private static boolean envFlag(String name) {
        String v = android.system.Os.getenv(name);
        return v != null && !v.isEmpty() && "0nNfF".indexOf(v.charAt(0)) < 0;
    }

    private void poll() {
        int due;
        try {
            due = nativePoll();
        } catch (UnsatisfiedLinkError e) {
            Log.w(TAG, "no native room light: " + e);
            return;
        }
        if (due < 0) {
            Log.i(TAG, "room light matching off");
            closeCamera();
            stopSensor();
            return;
        }
        if (!mSetUp) {
            mSetUp = true;
            setUp();
        }
        if (mCameraId == null && mListener != null && mLux >= 0f) {
            nativeSample(SOURCE_SENSOR, -1f, -1f, -1f, -1f, -1f, -1f, -1f, -1f, -1f, -1f, mLux);
        }
        if (due > 0 && mCameraId != null) {
            burst();
        }
        mHandler.postDelayed(mPoll, POLL_MS);
    }

    private void setUp() {
        mLog = envFlag("AURORA_XR_ROOM_LIGHT_LOG");
        String source = android.system.Os.getenv("AURORA_XR_ROOM_LIGHT_SOURCE");
        mSensors = (android.hardware.SensorManager) mContext.getSystemService(android.content.Context.SENSOR_SERVICE);
        if (mLog && mSensors != null) {
            for (android.hardware.Sensor s : mSensors.getSensorList(android.hardware.Sensor.TYPE_ALL)) {
                Log.i(TAG, "sensor " + s.getName() + " (" + s.getVendor() + "): type " + s.getType() + " "
                    + s.getStringType() + ", range " + s.getMaximumRange() + ", resolution " + s.getResolution());
            }
        }
        if (!"sensor".equals(source)) {
            pickCamera();
        }
        if (mCameraId == null && !"camera".equals(source)) {
            startSensor();
        }
        Log.i(TAG, "room light from " + (mCameraId != null ? "camera " + mCameraId + " at " + mSize
            : mListener != null ? "the light sensor" : "nothing: the room counts as normal"));
    }

    private boolean granted(String permission) {
        return mContext.checkSelfPermission(permission) == android.content.pm.PackageManager.PERMISSION_GRANTED;
    }

    /* A characteristic by name: Meta's vendor tags aren't public keys. */
    private static Object characteristic(CameraCharacteristics ch, String name) {
        for (CameraCharacteristics.Key<?> key : ch.getKeys()) {
            if (key.getName().equals(name)) {
                return ch.get(key);
            }
        }
        return null;
    }

    private static String describe(Object v) {
        if (v == null) {
            return "null";
        }
        if (v.getClass().isArray()) {
            StringBuilder sb = new StringBuilder("[");
            for (int i = 0; i < java.lang.reflect.Array.getLength(v); i++) {
                sb.append(i > 0 ? ", " : "").append(describe(java.lang.reflect.Array.get(v, i)));
            }
            return sb.append(']').toString();
        }
        return v.toString();
    }

    /* The left passthrough camera (camera_source 0, position 0), else the
     * first with YUV output; logs them all in log mode. */
    private void pickCamera() {
        if (!granted("horizonos.permission.HEADSET_CAMERA") && !granted(android.Manifest.permission.CAMERA)) {
            Log.i(TAG, "no camera permission");
            return;
        }
        CameraManager cm = (CameraManager) mContext.getSystemService(android.content.Context.CAMERA_SERVICE);
        if (cm == null) {
            return;
        }
        String pick = null, first = null;
        try {
            for (String id : cm.getCameraIdList()) {
                CameraCharacteristics ch = cm.getCameraCharacteristics(id);
                if (mLog) {
                    for (CameraCharacteristics.Key<?> key : ch.getKeys()) {
                        Log.i(TAG, "camera " + id + " " + key.getName() + " = " + describe(ch.get(key)));
                    }
                    StringBuilder keys = new StringBuilder();
                    for (CaptureResult.Key<?> key : ch.getAvailableCaptureResultKeys()) {
                        keys.append(keys.length() > 0 ? ", " : "").append(key.getName());
                    }
                    Log.i(TAG, "camera " + id + " result keys: " + keys);
                }
                android.hardware.camera2.params.StreamConfigurationMap map =
                    ch.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP);
                android.util.Size[] sizes = map != null ? map.getOutputSizes(android.graphics.ImageFormat.YUV_420_888) : null;
                if (sizes == null || sizes.length == 0) {
                    continue;
                }
                if (first == null) {
                    first = id;
                }
                Object source = characteristic(ch, "com.meta.extra_metadata.camera_source");
                Object position = characteristic(ch, "com.meta.extra_metadata.position");
                if (pick == null && source instanceof Number && ((Number) source).intValue() == 0
                        && (!(position instanceof Number) || ((Number) position).intValue() == 0)) {
                    pick = id;
                }
            }
            mCameraId = pick != null ? pick : first;
            if (mCameraId == null) {
                return;
            }
            CameraCharacteristics ch = cm.getCameraCharacteristics(mCameraId);
            /* The smallest at least 320 wide (Meta offers 320x240), else the smallest. */
            for (android.util.Size s : ch.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)
                    .getOutputSizes(android.graphics.ImageFormat.YUV_420_888)) {
                if (mSize == null) {
                    mSize = s;
                    continue;
                }
                boolean wide = s.getWidth() >= 320, bestWide = mSize.getWidth() >= 320;
                if (wide != bestWide ? wide
                        : (long) s.getWidth() * s.getHeight() < (long) mSize.getWidth() * mSize.getHeight()) {
                    mSize = s;
                }
            }
            android.util.Range<Integer>[] fps = ch.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES);
            if (fps != null) {
                for (android.util.Range<Integer> r : fps) {
                    if (mFps == null || r.getUpper() < mFps.getUpper()) {
                        mFps = r;
                    }
                }
            }
        } catch (Exception e) {
            Log.w(TAG, "cameras unavailable: " + e);
            mCameraId = null;
        }
    }

    private void startSensor() {
        if (mSensors == null) {
            return;
        }
        android.hardware.Sensor light = mSensors.getDefaultSensor(android.hardware.Sensor.TYPE_LIGHT);
        if (light == null) {
            return;
        }
        mListener = new android.hardware.SensorEventListener() {
            @Override
            public void onSensorChanged(android.hardware.SensorEvent event) {
                mLux = event.values[0];
            }

            @Override
            public void onAccuracyChanged(android.hardware.Sensor sensor, int accuracy) {}
        };
        mSensors.registerListener(mListener, light, android.hardware.SensorManager.SENSOR_DELAY_NORMAL, mHandler);
    }

    private void stopSensor() {
        if (mSensors != null && mListener != null) {
            mSensors.unregisterListener(mListener);
        }
        mListener = null;
    }

    private void burst() {
        if (mDevice != null || mReader != null) {
            return; /* one at a time */
        }
        mFrames = 0;
        mCount = 0;
        mSumR = mSumG = mSumB = mSumL = 0;
        mResult = null;
        mBurstStart = android.os.SystemClock.elapsedRealtime();
        try {
            CameraManager cm = (CameraManager) mContext.getSystemService(android.content.Context.CAMERA_SERVICE);
            mReader = android.media.ImageReader.newInstance(mSize.getWidth(), mSize.getHeight(),
                android.graphics.ImageFormat.YUV_420_888, 2);
            mReader.setOnImageAvailableListener(this::onImage, mHandler);
            mHandler.postDelayed(mTimeout, BURST_TIMEOUT_MS);
            cm.openCamera(mCameraId, new CameraDevice.StateCallback() {
                @Override
                public void onOpened(CameraDevice device) {
                    mDevice = device;
                    configure();
                }

                @Override
                public void onDisconnected(CameraDevice device) {
                    mDevice = device;
                    burstFailed("disconnected");
                }

                @Override
                public void onError(CameraDevice device, int error) {
                    mDevice = device;
                    burstFailed("error " + error);
                }
            }, mHandler);
        } catch (Exception e) {
            burstFailed(e.toString());
        }
    }

    private void configure() {
        final android.view.Surface surface = mReader.getSurface();
        try {
            java.util.List<android.hardware.camera2.params.OutputConfiguration> outputs =
                java.util.Collections.singletonList(new android.hardware.camera2.params.OutputConfiguration(surface));
            mDevice.createCaptureSession(new android.hardware.camera2.params.SessionConfiguration(
                android.hardware.camera2.params.SessionConfiguration.SESSION_REGULAR, outputs, mHandler::post,
                new CameraCaptureSession.StateCallback() {
                    @Override
                    public void onConfigured(CameraCaptureSession session) {
                        mSession = session;
                        try {
                            CaptureRequest.Builder b = mDevice.createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW);
                            b.addTarget(surface);
                            if (mFps != null) {
                                b.set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, mFps);
                            }
                            session.setRepeatingRequest(b.build(), new CameraCaptureSession.CaptureCallback() {
                                @Override
                                public void onCaptureCompleted(CameraCaptureSession s, CaptureRequest request,
                                        TotalCaptureResult result) {
                                    mResult = result;
                                }
                            }, mHandler);
                        } catch (Exception e) {
                            burstFailed(e.toString());
                        }
                    }

                    @Override
                    public void onConfigureFailed(CameraCaptureSession session) {
                        burstFailed("session not configured");
                    }
                }));
        } catch (Exception e) {
            burstFailed(e.toString());
        }
    }

    private static float linear(float c) {
        c = Math.min(Math.max(c, 0f), 1f);
        return c <= 0.04045f ? c / 12.92f : (float) Math.pow((c + 0.055f) / 1.055f, 2.4f);
    }

    private void onImage(android.media.ImageReader reader) {
        android.media.Image image = reader.acquireLatestImage();
        if (image == null) {
            return;
        }
        try {
            if (++mFrames > 1) {
                accumulate(image);
            }
        } finally {
            image.close();
        }
        if (mFrames >= BURST_FRAMES) {
            finishBurst();
        }
    }

    /* Full-range YCbCr (BT.601, as camera YUV is) to RGB, decoded to linear light. */
    private void accumulate(android.media.Image image) {
        android.media.Image.Plane[] p = image.getPlanes();
        java.nio.ByteBuffer y = p[0].getBuffer(), u = p[1].getBuffer(), v = p[2].getBuffer();
        int yRow = p[0].getRowStride(), yPix = p[0].getPixelStride();
        int uRow = p[1].getRowStride(), uPix = p[1].getPixelStride();
        int vRow = p[2].getRowStride(), vPix = p[2].getPixelStride();
        int w = image.getWidth(), h = image.getHeight();
        for (int row = h * 4 / 10; row < h * 9 / 10; row += 8) {
            for (int col = w * 2 / 10; col < w * 8 / 10; col += 8) {
                float l = y.get(row * yRow + col * yPix) & 0xff;
                float cb = (u.get((row / 2) * uRow + (col / 2) * uPix) & 0xff) - 128f;
                float cr = (v.get((row / 2) * vRow + (col / 2) * vPix) & 0xff) - 128f;
                float r = linear((l + 1.402f * cr) / 255f);
                float g = linear((l - 0.344136f * cb - 0.714136f * cr) / 255f);
                float b = linear((l + 1.772f * cb) / 255f);
                mSumR += r;
                mSumG += g;
                mSumB += b;
                mSumL += 0.2126f * r + 0.7152f * g + 0.0722f * b;
                mCount++;
            }
        }
    }

    private void finishBurst() {
        if (mCount == 0) {
            burstFailed("no pixels");
            return;
        }
        CaptureResult result = mResult;
        Long exposure = result != null ? result.get(CaptureResult.SENSOR_EXPOSURE_TIME) : null;
        Integer iso = result != null ? result.get(CaptureResult.SENSOR_SENSITIVITY) : null;
        android.hardware.camera2.params.RggbChannelVector wb =
            result != null ? result.get(CaptureResult.COLOR_CORRECTION_GAINS) : null;
        float luma = (float) (mSumL / mCount);
        float r = (float) (mSumR / mCount), g = (float) (mSumG / mCount), b = (float) (mSumB / mCount);
        float exposureMs = exposure != null ? exposure / 1e6f : -1f;
        nativeSample(SOURCE_CAMERA, luma, r, g, b, exposureMs, iso != null ? iso : -1f,
            wb != null ? wb.getRed() : -1f, wb != null ? wb.getGreenEven() : -1f,
            wb != null ? wb.getGreenOdd() : -1f, wb != null ? wb.getBlue() : -1f, -1f);
        if (mLog) {
            Log.i(TAG, String.format(java.util.Locale.ROOT,
                "burst: %d frames in %d ms, luminance %.4f, rgb %.4f/%.4f/%.4f, exposure %.2f ms, ISO %s, "
                    + "white balance %s", mFrames, android.os.SystemClock.elapsedRealtime() - mBurstStart, luma,
                r, g, b, exposureMs, iso, wb));
        }
        mFailures = 0;
        closeCamera();
    }

    private void burstFailed(String why) {
        closeCamera();
        Log.w(TAG, "camera burst failed: " + why);
        if (++mFailures >= MAX_FAILURES && mCameraId != null) {
            Log.w(TAG, "camera given up; the light sensor instead, if there is one");
            mCameraId = null;
            startSensor();
        }
    }

    private void closeCamera() {
        if (mHandler != null) {
            mHandler.removeCallbacks(mTimeout);
        }
        if (mSession != null) {
            mSession.close();
            mSession = null;
        }
        if (mDevice != null) {
            mDevice.close();
            mDevice = null;
        }
        if (mReader != null) {
            mReader.close();
            mReader = null;
        }
    }
}
