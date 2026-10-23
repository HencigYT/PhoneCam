package hu.phonecam;

import android.Manifest;
import android.app.Activity;
import android.app.AlertDialog;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.os.BatteryManager;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.graphics.Rect;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.hardware.camera2.CameraCharacteristics;
import android.hardware.camera2.CameraManager;
import android.hardware.camera2.CameraMetadata;
import android.hardware.camera2.params.StreamConfigurationMap;
import android.hardware.display.DisplayManager;
import android.media.MediaCodec;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.text.InputType;
import android.util.Range;
import android.util.Size;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.Spinner;
import android.widget.TextView;

import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

public class MainActivity extends Activity
        implements SurfaceHolder.Callback, CameraStreamer.Listener, NetClient.Listener {

    private static final int REQ_CAMERA = 1;
    private static final int[][] RESOLUTIONS = {{3840, 2160}, {2560, 1440}, {1920, 1080}, {1280, 720}};
    private static final int[] FPS_OPTIONS = {60, 30};
    private static final int[] BITRATES = {0, 6, 10, 15, 20, 30, 40, 60, 80};

    private Prefs prefs;
    private CameraStreamer streamer;
    private NetClient net;
    private final Handler ui = new Handler(Looper.getMainLooper());

    private AspectFrame aspectFrame;
    private SurfaceView surfaceView;
    private TextView statusText, infoText, zoomText;
    private SeekBar zoomBar;
    private Button torchButton;
    private View dimOverlay;
    private LinearLayout zoomRow;

    private StreamConfig activeConfig;
    private Size surfaceSize;
    private boolean pendingStart;
    private boolean resumed;
    private int pcCodecs = -1; // unknown until connected
    private String netText = "";
    private boolean netConnected;
    private String errorText;
    private int lastFrames, lastSent;
    private long lastBytes, lastStatsAt;
    private DisplayManager.DisplayListener displayListener;
    private int batteryLevel = -1;
    private boolean charging;
    private boolean batteryRegistered;
    private final BroadcastReceiver batteryReceiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            int level = intent.getIntExtra(BatteryManager.EXTRA_LEVEL, -1);
            int scale = intent.getIntExtra(BatteryManager.EXTRA_SCALE, 100);
            int status = intent.getIntExtra(BatteryManager.EXTRA_STATUS, -1);
            int pct = level >= 0 && scale > 0 ? level * 100 / scale : -1;
            boolean chg = status == BatteryManager.BATTERY_STATUS_CHARGING || status == BatteryManager.BATTERY_STATUS_FULL;
            if (pct != batteryLevel || chg != charging) {
                batteryLevel = pct;
                charging = chg;
                sendInfoSoon();
            }
        }
    };

    // ------------------------------------------------------------------ lifecycle

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        prefs = new Prefs(this);
        streamer = new CameraStreamer(this, this);
        net = new NetClient(this, this);
        buildUi();

        displayListener = new DisplayManager.DisplayListener() {
            @Override
            public void onDisplayAdded(int displayId) {
            }

            @Override
            public void onDisplayRemoved(int displayId) {
            }

            @Override
            public void onDisplayChanged(int displayId) {
                updateRotation();
            }
        };
        ((DisplayManager) getSystemService(Context.DISPLAY_SERVICE)).registerDisplayListener(displayListener, ui);
    }

    @Override
    protected void onDestroy() {
        ((DisplayManager) getSystemService(Context.DISPLAY_SERVICE)).unregisterDisplayListener(displayListener);
        super.onDestroy();
    }

    @Override
    protected void onResume() {
        super.onResume();
        resumed = true;
        hideSystemBars();
        if (!batteryRegistered) {
            registerReceiver(batteryReceiver, new IntentFilter(Intent.ACTION_BATTERY_CHANGED));
            batteryRegistered = true;
        }
        if (checkSelfPermission(Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(new String[]{Manifest.permission.CAMERA}, REQ_CAMERA);
            return;
        }
        startAll();
    }

    @Override
    protected void onPause() {
        resumed = false;
        ui.removeCallbacks(statsTick);
        ui.removeCallbacks(sendInfoNow);
        if (batteryRegistered) {
            unregisterReceiver(batteryReceiver);
            batteryRegistered = false;
        }
        streamer.stop();
        net.stop();
        super.onPause();
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] results) {
        if (requestCode != REQ_CAMERA) return;
        if (results.length > 0 && results[0] == PackageManager.PERMISSION_GRANTED) {
            if (resumed) startAll();
        } else {
            statusText.setText(Lang.t("Kamera engedély nélkül nem működik. Engedélyezd a beállításokban.", "Camera permission is required. Allow it in the settings."));
        }
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) hideSystemBars();
    }

    private void startAll() {
        net.setManualIp(prefs.manualIp);
        net.start();
        restartStream();
        lastStatsAt = System.currentTimeMillis();
        ui.removeCallbacks(statsTick);
        ui.postDelayed(statsTick, 1000);
    }

    private void hideSystemBars() {
        WindowInsetsController c = getWindow().getInsetsController();
        if (c == null) return;
        c.hide(WindowInsets.Type.systemBars());
        c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
    }

    // ------------------------------------------------------------------ stream setup

    private void restartStream() {
        streamer.stop();
        if (!resumed || checkSelfPermission(Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) return;
        StreamConfig cfg;
        try {
            cfg = resolveConfig();
        } catch (Exception e) {
            showError(Lang.t("Kamera nem elérhető: ", "Camera not available: ") + e.getMessage());
            return;
        }
        errorText = null;
        activeConfig = cfg;
        aspectFrame.setRatio((double) cfg.width / cfg.height);
        setupZoomBar(cfg);
        torchButton.setVisibility(cfg.hasFlash ? View.VISIBLE : View.GONE);
        torchButton.setText(Lang.t("Vaku", "Flash"));
        if (surfaceSize == null || surfaceSize.getWidth() != cfg.previewWidth
                || surfaceSize.getHeight() != cfg.previewHeight) {
            // Wait for surfaceChanged with the new buffer size before opening the camera.
            pendingStart = true;
            surfaceView.getHolder().setFixedSize(cfg.previewWidth, cfg.previewHeight);
            return;
        }
        startStreamer();
    }

    private void startStreamer() {
        pendingStart = false;
        StreamConfig cfg = activeConfig;
        if (cfg == null || !resumed) return;
        cfg.rotation = computeRotation(cfg);
        net.setConfig(cfg);
        streamer.start(surfaceView.getHolder().getSurface(), cfg);
        updateInfo();
        sendInfoSoon();
    }

    private StreamConfig resolveConfig() throws Exception {
        CameraManager cm = (CameraManager) getSystemService(Context.CAMERA_SERVICE);
        String[] ids = cm.getCameraIdList();
        if (ids.length == 0) throw new IllegalStateException(Lang.t("nincs kamera", "no camera"));
        String id = null;
        for (String c : ids) if (c.equals(prefs.cameraId)) id = c;
        if (id == null) {
            id = ids[0];
            for (String c : ids) {
                Integer f = cm.getCameraCharacteristics(c).get(CameraCharacteristics.LENS_FACING);
                if (f != null && f == CameraMetadata.LENS_FACING_BACK) {
                    id = c;
                    break;
                }
            }
        }
        CameraCharacteristics ch = cm.getCameraCharacteristics(id);
        StreamConfigurationMap map = ch.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP);

        StreamConfig cfg = new StreamConfig();
        cfg.cameraId = id;
        Integer facing = ch.get(CameraCharacteristics.LENS_FACING);
        cfg.facing = facing != null ? facing : CameraMetadata.LENS_FACING_BACK;
        Integer orientation = ch.get(CameraCharacteristics.SENSOR_ORIENTATION);
        cfg.sensorOrientation = orientation != null ? orientation : 90;

        // Resolution: the chosen one if the camera and encoder can do it, else the next lower.
        int codec = prefs.codec;
        if (codec == StreamConfig.CODEC_HEVC && pcCodecs >= 0 && (pcCodecs & 2) == 0) codec = StreamConfig.CODEC_H264;
        Size size = null;
        for (int[] r : RESOLUTIONS) {
            if (r[0] * r[1] > prefs.width * prefs.height) continue;
            if (supportsSize(map, r[0], r[1])) {
                size = new Size(r[0], r[1]);
                break;
            }
        }
        if (size == null) size = map.getOutputSizes(MediaCodec.class)[0];
        cfg.width = size.getWidth();
        cfg.height = size.getHeight();

        cfg.fps = 30;
        cfg.note = null;
        if (prefs.fps >= 60) {
            // 60 fps: a normal session if the camera offers it, otherwise the
            // constrained high-speed session (Samsung gives 3rd-party apps 60+ fps
            // only there). Keeping 60 fps wins over resolution.
            for (int[] r : RESOLUTIONS) {
                if (r[0] * r[1] > size.getWidth() * size.getHeight()) continue;
                Size s = new Size(r[0], r[1]);
                if (supportsSize(map, r[0], r[1]) && fpsRangeFor(ch, 60) != null && canDoFps(map, s, 60)) {
                    size = s;
                    cfg.fps = 60;
                    break;
                }
                Range<Integer> hs = highSpeedRange(ch, map, s);
                if (hs != null) {
                    size = s;
                    cfg.fps = 60;
                    cfg.highSpeed = true;
                    cfg.fpsRange = hs;
                    break;
                }
            }
            if (cfg.fps == 60 && (size.getWidth() != cfg.width || size.getHeight() != cfg.height))
                cfg.note = Lang.t(cfg.width + "×" + cfg.height + "-ban nincs 60 fps, ezért " + size.getHeight() + "p60 megy", "No 60 fps at " + cfg.width + "×" + cfg.height + ", using " + size.getHeight() + "p60");
            else if (cfg.fps != 60)
                cfg.note = Lang.t("Ez a kamera nem ad 60 fps-t, 30 fps megy", "This camera has no 60 fps, using 30 fps");
            cfg.width = size.getWidth();
            cfg.height = size.getHeight();
        }
        if (!cfg.highSpeed) {
            cfg.fpsRange = fpsRangeFor(ch, cfg.fps);
            if (cfg.fpsRange == null) cfg.fpsRange = new Range<>(15, 30);
        }
        cfg.captureFps = cfg.fpsRange.getUpper();

        if (CameraStreamer.findEncoder(codec, cfg.width, cfg.height, cfg.fps) == null) {
            codec = StreamConfig.CODEC_H264;
            if (CameraStreamer.findEncoder(codec, cfg.width, cfg.height, cfg.fps) == null) {
                cfg.width = 1920;
                cfg.height = 1080;
                cfg.fps = 30;
                cfg.highSpeed = false;
                cfg.fpsRange = fpsRangeFor(ch, 30) != null ? fpsRangeFor(ch, 30) : new Range<>(15, 30);
                cfg.captureFps = cfg.fpsRange.getUpper();
            }
        }
        cfg.codec = codec;

        int mbps = prefs.bitrateMbps;
        if (mbps <= 0) {
            double auto = (double) cfg.width * cfg.height * cfg.fps * 0.16 / 1e6;
            if (codec == StreamConfig.CODEC_HEVC) auto *= 0.7;
            mbps = (int) Math.round(Math.max(6, Math.min(60, auto)));
        }
        cfg.bitrate = mbps * 1_000_000;

        // High-speed sessions want preview and video at the same size.
        Size preview = cfg.highSpeed ? new Size(cfg.width, cfg.height)
                : choosePreviewSize(map, cfg.width, cfg.height);
        cfg.previewWidth = preview.getWidth();
        cfg.previewHeight = preview.getHeight();

        Integer tsSource = ch.get(CameraCharacteristics.SENSOR_INFO_TIMESTAMP_SOURCE);
        cfg.realtimeTimestamps = tsSource != null && tsSource == CameraMetadata.SENSOR_INFO_TIMESTAMP_SOURCE_REALTIME;
        cfg.stabilization = !cfg.highSpeed && prefs.stabilization && contains(
                ch.get(CameraCharacteristics.CONTROL_AVAILABLE_VIDEO_STABILIZATION_MODES),
                CameraMetadata.CONTROL_VIDEO_STABILIZATION_MODE_ON);
        cfg.hasOis = contains(ch.get(CameraCharacteristics.LENS_INFO_AVAILABLE_OPTICAL_STABILIZATION),
                CameraMetadata.LENS_OPTICAL_STABILIZATION_MODE_ON);
        Boolean flash = ch.get(CameraCharacteristics.FLASH_INFO_AVAILABLE);
        cfg.hasFlash = flash != null && flash;
        cfg.continuousVideoAf = contains(ch.get(CameraCharacteristics.CONTROL_AF_AVAILABLE_MODES),
                CameraMetadata.CONTROL_AF_MODE_CONTINUOUS_VIDEO);
        Integer afRegions = ch.get(CameraCharacteristics.CONTROL_MAX_REGIONS_AF);
        Integer aeRegions = ch.get(CameraCharacteristics.CONTROL_MAX_REGIONS_AE);
        cfg.maxAfRegions = afRegions != null ? afRegions : 0;
        cfg.maxAeRegions = aeRegions != null ? aeRegions : 0;
        cfg.activeArray = ch.get(CameraCharacteristics.SENSOR_INFO_ACTIVE_ARRAY_SIZE);
        Range<Float> zoom = ch.get(CameraCharacteristics.CONTROL_ZOOM_RATIO_RANGE);
        if (zoom != null) {
            cfg.minZoom = zoom.getLower();
            cfg.maxZoom = Math.min(zoom.getUpper(), 30f);
        }
        return cfg;
    }

    private static boolean contains(int[] values, int v) {
        if (values == null) return false;
        for (int x : values) if (x == v) return true;
        return false;
    }

    private static boolean supportsSize(StreamConfigurationMap map, int w, int h) {
        for (Size s : map.getOutputSizes(MediaCodec.class))
            if (s.getWidth() == w && s.getHeight() == h) return true;
        return false;
    }

    private static boolean canDoFps(StreamConfigurationMap map, Size size, int fps) {
        try {
            long minDuration = map.getOutputMinFrameDuration(MediaCodec.class, size);
            return minDuration <= 1_000_000_000L / fps + 100_000;
        } catch (Exception e) {
            return fps <= 30;
        }
    }

    /** Prefer a fixed [fps, fps] range so the exposure never drops the frame rate. */
    private static Range<Integer> fpsRangeFor(CameraCharacteristics ch, int fps) {
        Range<Integer>[] ranges = ch.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES);
        if (ranges == null) return null;
        Range<Integer> best = null;
        for (Range<Integer> r : ranges) {
            if (r.getUpper() != fps) continue;
            if (best == null || r.getLower() > best.getLower()) best = r;
        }
        return best;
    }

    /**
     * Fixed high-speed range (the lowest one that is at least 60 fps) for this
     * size, or null if the camera has no constrained high-speed mode for it.
     */
    private static Range<Integer> highSpeedRange(CameraCharacteristics ch, StreamConfigurationMap map, Size size) {
        if (!contains(ch.get(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES),
                CameraMetadata.REQUEST_AVAILABLE_CAPABILITIES_CONSTRAINED_HIGH_SPEED_VIDEO)) return null;
        Range<Integer> best = null;
        try {
            for (Range<Integer> r : map.getHighSpeedVideoFpsRangesFor(size)) {
                if (!r.getLower().equals(r.getUpper()) || r.getUpper() < 60) continue;
                if (best == null || r.getUpper() < best.getUpper()) best = r;
            }
        } catch (IllegalArgumentException e) {
            return null; // size not supported in high-speed mode
        }
        return best;
    }

    /** Camera frame-rate capabilities, shown in the settings for diagnostics. */
    private static String describeFps(CameraCharacteristics ch, StreamConfigurationMap map) {
        StringBuilder sb = new StringBuilder(Lang.t("Normál mód fps: ", "Normal mode fps: "));
        Range<Integer>[] ranges = ch.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES);
        if (ranges != null) for (Range<Integer> r : ranges) sb.append(r).append(' ');
        sb.append(Lang.t("\nNagysebességű mód: ", "\nHigh-speed mode: "));
        if (!contains(ch.get(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES),
                CameraMetadata.REQUEST_AVAILABLE_CAPABILITIES_CONSTRAINED_HIGH_SPEED_VIDEO)) {
            sb.append(Lang.t("nincs", "none"));
        } else {
            for (Size s : map.getHighSpeedVideoSizes()) {
                sb.append(s.getWidth()).append('×').append(s.getHeight()).append(' ');
                for (Range<Integer> r : map.getHighSpeedVideoFpsRangesFor(s)) sb.append(r);
                sb.append("  ");
            }
        }
        return sb.toString();
    }

    private static Size choosePreviewSize(StreamConfigurationMap map, int w, int h) {
        Size best = null;
        double aspect = (double) w / h;
        for (Size s : map.getOutputSizes(SurfaceHolder.class)) {
            if (Math.abs((double) s.getWidth() / s.getHeight() - aspect) > 0.01) continue;
            if ((long) s.getWidth() * s.getHeight() > 1920L * 1080) continue;
            if (best == null || s.getWidth() > best.getWidth()) best = s;
        }
        return best != null ? best : new Size(Math.min(w, 1920), Math.min(h, 1080));
    }

    private int displayDegrees() {
        int rot = getDisplay() != null ? getDisplay().getRotation() : Surface.ROTATION_90;
        switch (rot) {
            case Surface.ROTATION_90:
                return 90;
            case Surface.ROTATION_180:
                return 180;
            case Surface.ROTATION_270:
                return 270;
            default:
                return 0;
        }
    }

    /** Rotation that turns the raw sensor image upright for the current way the phone is held. */
    private int computeRotation(StreamConfig cfg) {
        int deg = displayDegrees();
        if (cfg.facing == CameraMetadata.LENS_FACING_FRONT) return (cfg.sensorOrientation + deg) % 360;
        return (cfg.sensorOrientation - deg + 360) % 360;
    }

    private void updateRotation() {
        StreamConfig cfg = activeConfig;
        if (cfg == null) return;
        int rot = computeRotation(cfg);
        if (rot != cfg.rotation) {
            cfg.rotation = rot;
            net.sendOrientation(rot);
        }
    }

    // ------------------------------------------------------------------ callbacks

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        surfaceSize = new Size(width, height);
        StreamConfig cfg = activeConfig;
        if (pendingStart && cfg != null && width == cfg.previewWidth && height == cfg.previewHeight) startStreamer();
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        surfaceSize = null;
        if (streamer.isRunning()) {
            streamer.stop();
            pendingStart = true;
        }
    }

    @Override
    public void onEncodedFrame(byte[] data, long ptsUs, boolean keyFrame) {
        net.sendFrame(data, ptsUs, keyFrame);
    }

    @Override
    public void onStreamerError(String message) {
        ui.post(() -> showError(message));
    }

    @Override
    public void onNetStatus(String text, boolean connected) {
        ui.post(() -> {
            netText = text;
            netConnected = connected;
            updateInfo();
        });
    }

    @Override
    public void onConnected(int codecMask) {
        ui.post(() -> {
            boolean changed = pcCodecs != codecMask;
            pcCodecs = codecMask;
            // The PC cannot decode HEVC: fall back to H.264 transparently.
            if (changed && activeConfig != null && activeConfig.codec == StreamConfig.CODEC_HEVC
                    && (codecMask & 2) == 0) {
                restartStream();
            }
            sendInfoSoon();
        });
    }

    @Override
    public void onKeyFrameRequested() {
        streamer.requestKeyFrame();
    }

    @Override
    public void onSettingsFromPc(String settings) {
        ui.post(() -> applyPcSettings(settings));
    }

    /** Settings changed in the PC window: same effect as changing them here. */
    private void applyPcSettings(String settings) {
        boolean restart = false;
        for (String line : settings.split("\n")) {
            int eq = line.indexOf('=');
            if (eq <= 0) continue;
            String key = line.substring(0, eq).trim(), value = line.substring(eq + 1).trim();
            try {
                switch (key) {
                    case "camera":
                        restart |= !value.equals(prefs.cameraId);
                        prefs.cameraId = value;
                        break;
                    case "width":
                        restart |= prefs.width != Integer.parseInt(value);
                        prefs.width = Integer.parseInt(value);
                        break;
                    case "height":
                        restart |= prefs.height != Integer.parseInt(value);
                        prefs.height = Integer.parseInt(value);
                        break;
                    case "fps":
                        restart |= prefs.fps != Integer.parseInt(value);
                        prefs.fps = Integer.parseInt(value);
                        break;
                    case "codec":
                        restart |= prefs.codec != Integer.parseInt(value);
                        prefs.codec = Integer.parseInt(value);
                        break;
                    case "bitrate":
                        restart |= prefs.bitrateMbps != Integer.parseInt(value);
                        prefs.bitrateMbps = Integer.parseInt(value);
                        break;
                    case "stab":
                        restart |= prefs.stabilization != value.equals("1");
                        prefs.stabilization = value.equals("1");
                        break;
                    case "torch":
                        boolean on = streamer.setTorch(value.equals("1"));
                        torchButton.setText(on ? Lang.t("Vaku: BE", "Flash: ON") : Lang.t("Vaku", "Flash"));
                        break;
                    case "zoom":
                        float z = Float.parseFloat(value);
                        streamer.setZoom(z);
                        showZoom(streamer.getZoom());
                        break;
                }
            } catch (NumberFormatException ignored) {
            }
        }
        if (restart) {
            prefs.save();
            restartStream();
        }
        sendInfoSoon();
    }

    private final Runnable sendInfoNow = () -> {
        if (net.isConnected()) net.sendInfo(buildInfo());
    };

    /** Coalesces bursts (zoom dragging, battery events) into one report. */
    private void sendInfoSoon() {
        ui.removeCallbacks(sendInfoNow);
        ui.postDelayed(sendInfoNow, 150);
    }

    /** Report for the PC window: battery, current settings and what each camera supports. */
    private String buildInfo() {
        StringBuilder sb = new StringBuilder();
        StreamConfig cfg = activeConfig;
        sb.append("battery=").append(batteryLevel).append('\n');
        sb.append("charging=").append(charging ? 1 : 0).append('\n');
        sb.append("camera=").append(cfg != null ? cfg.cameraId : prefs.cameraId).append('\n');
        sb.append("width=").append(cfg != null ? cfg.width : prefs.width).append('\n');
        sb.append("height=").append(cfg != null ? cfg.height : prefs.height).append('\n');
        sb.append("fps=").append(cfg != null ? cfg.fps : prefs.fps).append('\n');
        sb.append("codec=").append(prefs.codec).append('\n');
        sb.append("bitrate=").append(prefs.bitrateMbps).append('\n');
        sb.append("stab=").append(prefs.stabilization ? 1 : 0).append('\n');
        sb.append("torch=").append(streamer.isTorch() ? 1 : 0).append('\n');
        sb.append("flash=").append(cfg != null && cfg.hasFlash ? 1 : 0).append('\n');
        sb.append(String.format(Locale.US, "zoom=%.2f\nzoommin=%.2f\nzoommax=%.2f\n", streamer.getZoom(),
                cfg != null ? cfg.minZoom : 1f, cfg != null ? cfg.maxZoom : 1f));
        try {
            CameraManager cm = (CameraManager) getSystemService(Context.CAMERA_SERVICE);
            for (String id : cm.getCameraIdList()) {
                CameraCharacteristics ch = cm.getCameraCharacteristics(id);
                StreamConfigurationMap map = ch.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP);
                StringBuilder res = new StringBuilder();
                boolean any60 = fpsRangeFor(ch, 60) != null;
                for (int[] r : RESOLUTIONS) {
                    if (!supportsSize(map, r[0], r[1])) continue;
                    res.append(res.length() > 0 ? "," : "").append(r[0]).append('x').append(r[1]);
                    any60 |= highSpeedRange(ch, map, new Size(r[0], r[1])) != null;
                }
                sb.append("cam=").append(id).append('|').append(cameraLabel(cm, id).replace('|', '/'))
                        .append('|').append(res).append('|').append(any60 ? "60,30" : "30").append('\n');
            }
        } catch (Exception ignored) {
        }
        return sb.toString();
    }

    private void showError(String message) {
        errorText = message;
        updateInfo();
    }

    private int ticks;
    private final Runnable statsTick = new Runnable() {
        @Override
        public void run() {
            updateInfo();
            if (++ticks % 20 == 0) sendInfoSoon(); // keeps the battery fresh on the PC
            ui.postDelayed(this, 1000);
        }
    };

    private void updateInfo() {
        statusText.setText((netConnected ? "● " : "○ ") + (netText.isEmpty() ? Lang.t("Indítás…", "Starting…") : netText));
        statusText.setTextColor(netConnected ? Color.rgb(110, 231, 140) : Color.rgb(250, 204, 21));

        long now = System.currentTimeMillis();
        double secs = Math.max(0.2, (now - lastStatsAt) / 1000.0);
        int frames = streamer.encodedFrames.get();
        int sent = net.sentFrames.get();
        long bytes = net.sentBytes.get();
        double encFps = (frames - lastFrames) / secs;
        double sentFps = (sent - lastSent) / secs;
        double mbps = (bytes - lastBytes) * 8 / secs / 1e6;
        lastFrames = frames;
        lastSent = sent;
        lastBytes = bytes;
        lastStatsAt = now;

        StringBuilder sb = new StringBuilder();
        StreamConfig cfg = activeConfig;
        if (cfg != null) {
            sb.append(String.format(Locale.US, "%d×%d · %d fps · %s · %d Mbps", cfg.width, cfg.height, cfg.fps,
                    cfg.codecName(), cfg.bitrate / 1_000_000));
            if (cfg.highSpeed)
                sb.append(String.format(Locale.US, Lang.t(" · nagysebességű mód (%d→%d fps)", " · high-speed mode (%d→%d fps)"), cfg.captureFps, cfg.fps));
            if (cfg.note != null) sb.append("\n").append(cfg.note);
            sb.append(String.format(Locale.US, Lang.t("\nKódolt: %.0f fps", "\nEncoded: %.0f fps"), encFps));
            if (netConnected) sb.append(String.format(Locale.US, Lang.t(" · küldés: %.0f fps, %.1f Mbps", " · sending: %.0f fps, %.1f Mbps"), sentFps, mbps));
            if (prefs.codec == StreamConfig.CODEC_HEVC && cfg.codec == StreamConfig.CODEC_H264)
                sb.append(Lang.t("\nA PC nem tud HEVC-t, H.264 megy helyette", "\nThe PC cannot decode HEVC, using H.264"));
        }
        if (errorText != null) sb.append("\n⚠ ").append(errorText);
        infoText.setText(sb.toString());
    }

    // ------------------------------------------------------------------ UI

    private int dp(float v) {
        return (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, getResources().getDisplayMetrics());
    }

    private GradientDrawable pill(int color) {
        GradientDrawable d = new GradientDrawable();
        d.setColor(color);
        d.setCornerRadius(dp(14));
        return d;
    }

    private Button makeButton(String text, View.OnClickListener onClick) {
        Button b = new Button(this);
        b.setText(text);
        b.setAllCaps(false);
        b.setTextColor(Color.WHITE);
        b.setTextSize(14);
        b.setBackground(pill(0xAA222222));
        b.setPadding(dp(14), 0, dp(14), 0);
        b.setMinWidth(dp(120));
        b.setOnClickListener(onClick);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, dp(44));
        lp.topMargin = dp(8);
        b.setLayoutParams(lp);
        return b;
    }

    private void buildUi() {
        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(Color.BLACK);

        aspectFrame = new AspectFrame(this);
        surfaceView = new SurfaceView(this);
        surfaceView.getHolder().addCallback(this);
        surfaceView.setOnTouchListener(this::onPreviewTouch);
        aspectFrame.addView(surfaceView, new FrameLayout.LayoutParams(-1, -1));
        root.addView(aspectFrame, new FrameLayout.LayoutParams(-2, -2, Gravity.CENTER));

        LinearLayout status = new LinearLayout(this);
        status.setOrientation(LinearLayout.VERTICAL);
        status.setBackground(pill(0x99000000));
        status.setPadding(dp(12), dp(8), dp(12), dp(8));
        statusText = new TextView(this);
        statusText.setTextSize(15);
        statusText.setTypeface(Typeface.DEFAULT_BOLD);
        infoText = new TextView(this);
        infoText.setTextSize(12);
        infoText.setTextColor(0xFFDDDDDD);
        status.addView(statusText);
        status.addView(infoText);
        FrameLayout.LayoutParams slp = new FrameLayout.LayoutParams(-2, -2, Gravity.TOP | Gravity.START);
        slp.setMargins(dp(16), dp(12), dp(16), 0);
        root.addView(status, slp);

        LinearLayout buttons = new LinearLayout(this);
        buttons.setOrientation(LinearLayout.VERTICAL);
        buttons.setGravity(Gravity.END);
        buttons.addView(makeButton(Lang.t("Beállítások", "Settings"), v -> showSettings()));
        buttons.addView(makeButton(Lang.t("Kamera váltás", "Switch camera"), v -> switchCamera()));
        torchButton = makeButton(Lang.t("Vaku", "Flash"), v -> {
            boolean on = streamer.toggleTorch();
            ((Button) v).setText(on ? Lang.t("Vaku: BE", "Flash: ON") : Lang.t("Vaku", "Flash"));
            sendInfoSoon();
        });
        buttons.addView(torchButton);
        buttons.addView(makeButton(Lang.t("Képernyő sötétítése", "Dim screen"), v -> setDimmed(true)));
        FrameLayout.LayoutParams blp = new FrameLayout.LayoutParams(-2, -2, Gravity.END | Gravity.CENTER_VERTICAL);
        blp.setMargins(0, 0, dp(16), 0);
        root.addView(buttons, blp);

        zoomRow = new LinearLayout(this);
        zoomRow.setOrientation(LinearLayout.HORIZONTAL);
        zoomRow.setGravity(Gravity.CENTER_VERTICAL);
        zoomRow.setBackground(pill(0x99000000));
        zoomRow.setPadding(dp(12), dp(4), dp(12), dp(4));
        zoomText = new TextView(this);
        zoomText.setTextColor(Color.WHITE);
        zoomText.setTextSize(14);
        zoomText.setMinWidth(dp(52));
        zoomBar = new SeekBar(this);
        zoomBar.setMax(1000);
        zoomRow.addView(zoomText);
        zoomRow.addView(zoomBar, new LinearLayout.LayoutParams(dp(320), -2));
        FrameLayout.LayoutParams zlp = new FrameLayout.LayoutParams(-2, -2, Gravity.BOTTOM | Gravity.CENTER_HORIZONTAL);
        zlp.bottomMargin = dp(16);
        root.addView(zoomRow, zlp);

        dimOverlay = new View(this);
        dimOverlay.setBackgroundColor(Color.BLACK);
        dimOverlay.setVisibility(View.GONE);
        dimOverlay.setOnClickListener(v -> setDimmed(false));
        root.addView(dimOverlay, new FrameLayout.LayoutParams(-1, -1));

        setContentView(root);
    }

    private void setupZoomBar(StreamConfig cfg) {
        if (cfg.maxZoom <= cfg.minZoom + 0.01f) {
            zoomRow.setVisibility(View.GONE);
            return;
        }
        zoomRow.setVisibility(View.VISIBLE);
        final double lo = Math.log(cfg.minZoom), hi = Math.log(cfg.maxZoom);
        float current = Math.max(cfg.minZoom, Math.min(cfg.maxZoom, streamer.getZoom()));
        zoomBar.setOnSeekBarChangeListener(null);
        zoomBar.setProgress((int) Math.round((Math.log(current) - lo) / (hi - lo) * 1000));
        zoomText.setText(String.format(Locale.US, "%.1f×", current));
        zoomBar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser) {
                if (!fromUser) return;
                float z = (float) Math.exp(lo + (hi - lo) * progress / 1000.0);
                if (Math.abs(z - 1f) < 0.04f) z = 1f; // snap to the main lens
                zoomText.setText(String.format(Locale.US, "%.1f×", z));
                streamer.setZoom(z);
                sendInfoSoon();
            }

            @Override
            public void onStartTrackingTouch(SeekBar seekBar) {
            }

            @Override
            public void onStopTrackingTouch(SeekBar seekBar) {
            }
        });
    }

    /** Moves the zoom bar to a zoom set from the PC. */
    private void showZoom(float z) {
        StreamConfig cfg = activeConfig;
        if (cfg == null || cfg.maxZoom <= cfg.minZoom + 0.01f) return;
        double lo = Math.log(cfg.minZoom), hi = Math.log(cfg.maxZoom);
        zoomBar.setProgress((int) Math.round((Math.log(z) - lo) / (hi - lo) * 1000));
        zoomText.setText(String.format(Locale.US, "%.1f×", z));
    }

    private boolean onPreviewTouch(View v, MotionEvent e) {
        if (e.getAction() != MotionEvent.ACTION_UP || activeConfig == null) return true;
        float vx = e.getX() / v.getWidth(), vy = e.getY() / v.getHeight();
        // Map the tap from screen space to sensor space (landscape: 0 or 180 degrees apart).
        boolean upsideDown = activeConfig.rotation == 180;
        float sx = upsideDown ? 1 - vx : vx;
        float sy = upsideDown ? 1 - vy : vy;
        if (activeConfig.facing == CameraMetadata.LENS_FACING_FRONT) sx = 1 - sx; // preview is mirrored
        streamer.focusAt(sx, sy);
        return true;
    }

    private void setDimmed(boolean dim) {
        dimOverlay.setVisibility(dim ? View.VISIBLE : View.GONE);
        WindowManager.LayoutParams lp = getWindow().getAttributes();
        lp.screenBrightness = dim ? 0.01f : WindowManager.LayoutParams.BRIGHTNESS_OVERRIDE_NONE;
        getWindow().setAttributes(lp);
    }

    private void switchCamera() {
        try {
            CameraManager cm = (CameraManager) getSystemService(Context.CAMERA_SERVICE);
            String[] ids = cm.getCameraIdList();
            if (ids.length < 2) return;
            String current = activeConfig != null ? activeConfig.cameraId : prefs.cameraId;
            int idx = 0;
            for (int i = 0; i < ids.length; i++) if (ids[i].equals(current)) idx = i;
            prefs.cameraId = ids[(idx + 1) % ids.length];
            prefs.save();
            restartStream();
        } catch (Exception e) {
            showError(e.getMessage());
        }
    }

    // ------------------------------------------------------------------ settings dialog

    private static final class Choice {
        final String label;
        final int a, b;

        Choice(String label, int a, int b) {
            this.label = label;
            this.a = a;
            this.b = b;
        }

        @Override
        public String toString() {
            return label;
        }
    }

    private TextView label(String text) {
        TextView t = new TextView(this);
        t.setText(text);
        t.setTextSize(13);
        t.setPadding(0, dp(12), 0, dp(2));
        return t;
    }

    private Spinner spinner(List<?> items, int selected) {
        Spinner s = new Spinner(this);
        ArrayAdapter<?> a = new ArrayAdapter<>(this, android.R.layout.simple_spinner_dropdown_item, items);
        s.setAdapter(a);
        if (selected >= 0 && selected < items.size()) s.setSelection(selected);
        return s;
    }

    private String cameraLabel(CameraManager cm, String id) {
        try {
            CameraCharacteristics ch = cm.getCameraCharacteristics(id);
            Integer f = ch.get(CameraCharacteristics.LENS_FACING);
            String side = f == null ? Lang.t("Kamera", "Camera") : f == CameraMetadata.LENS_FACING_BACK ? Lang.t("Hátsó", "Back")
                    : f == CameraMetadata.LENS_FACING_FRONT ? Lang.t("Előlapi", "Front") : Lang.t("Külső", "External");
            float[] fl = ch.get(CameraCharacteristics.LENS_INFO_AVAILABLE_FOCAL_LENGTHS);
            String extra = fl != null && fl.length > 0 ? String.format(Locale.US, ", %.1f mm", fl[0]) : "";
            if (contains(ch.get(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES),
                    CameraMetadata.REQUEST_AVAILABLE_CAPABILITIES_LOGICAL_MULTI_CAMERA))
                extra += Lang.t(", több lencse (zoommal vált)", ", multi-lens (zoom switches)");
            return side + " #" + id + extra;
        } catch (Exception e) {
            return Lang.t("Kamera #", "Camera #") + id;
        }
    }

    private void showSettings() {
        final CameraManager cm = (CameraManager) getSystemService(Context.CAMERA_SERVICE);
        final List<Choice> cameras = new ArrayList<>();
        int camSel = 0;
        try {
            String current = activeConfig != null ? activeConfig.cameraId : prefs.cameraId;
            for (String id : cm.getCameraIdList()) {
                if (id.equals(current)) camSel = cameras.size();
                cameras.add(new Choice(cameraLabel(cm, id), 0, 0));
            }
        } catch (Exception ignored) {
        }
        final String[] cameraIds;
        try {
            cameraIds = cm.getCameraIdList();
        } catch (Exception e) {
            return;
        }

        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dp(20), dp(4), dp(20), dp(8));

        final Spinner langSpin = spinner(java.util.Arrays.asList("Magyar", "English"), Lang.english ? 1 : 0);
        final Spinner camSpin = spinner(cameras, camSel);
        final Spinner resSpin = new Spinner(this);
        final Spinner fpsSpin = new Spinner(this);

        List<Choice> codecs = new ArrayList<>();
        boolean noHevc = pcCodecs >= 0 && (pcCodecs & 2) == 0;
        codecs.add(new Choice(Lang.t("H.264 (AVC) – legjobb kompatibilitás", "H.264 (AVC) – best compatibility"), StreamConfig.CODEC_H264, 0));
        codecs.add(new Choice(Lang.t("H.265 (HEVC) – jobb kép ugyanannyi sávszélen", "H.265 (HEVC) – better image at the same bitrate") + (noHevc ? Lang.t(" (a PC nem tudja)", " (PC cannot decode)") : ""),
                StreamConfig.CODEC_HEVC, 0));
        final Spinner codecSpin = spinner(codecs, prefs.codec == StreamConfig.CODEC_HEVC ? 1 : 0);

        List<Choice> rates = new ArrayList<>();
        int rateSel = 0;
        for (int r : BITRATES) {
            if (r == prefs.bitrateMbps) rateSel = rates.size();
            rates.add(new Choice(r == 0 ? Lang.t("Automatikus", "Automatic") : r + " Mbps", r, 0));
        }
        final Spinner rateSpin = spinner(rates, rateSel);

        final CheckBox stab = new CheckBox(this);
        stab.setText(Lang.t("Képstabilizálás (EIS) – nyugodtabb kép, de több késés", "Stabilization (EIS) – steadier image, more delay"));
        stab.setChecked(prefs.stabilization);

        final EditText ipEdit = new EditText(this);
        ipEdit.setHint(Lang.t("pl. 192.168.1.10 – üresen automatikus", "e.g. 192.168.1.10 – empty = automatic"));
        ipEdit.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI);
        ipEdit.setText(prefs.manualIp);

        box.addView(label("Nyelv / Language"));
        box.addView(langSpin);
        box.addView(label(Lang.t("Kamera", "Camera")));
        box.addView(camSpin);
        box.addView(label(Lang.t("Felbontás", "Resolution")));
        box.addView(resSpin);
        box.addView(label(Lang.t("Képkocka/mp", "Frame rate")));
        box.addView(fpsSpin);
        box.addView(label(Lang.t("Kodek", "Codec")));
        box.addView(codecSpin);
        box.addView(label(Lang.t("Bitráta", "Bitrate")));
        box.addView(rateSpin);
        box.addView(stab);
        box.addView(label(Lang.t("PC IP címe (ha az automatikus keresés nem találja)", "PC IP address (if it is not found automatically)")));
        box.addView(ipEdit);
        TextView hint = new TextView(this);
        hint.setTextSize(12);
        hint.setPadding(0, dp(12), 0, 0);
        hint.setText(Lang.t("USB: kapcsold be az USB hibakeresést (Fejlesztői beállítások), dugd be a kábelt, "
                + "és a PhoneCam magától átvált – ez a legkisebb késés.",
                "USB: enable USB debugging (Developer options) and plug in the cable – PhoneCam switches "
                + "over by itself, this has the lowest delay."));
        box.addView(hint);
        final TextView fpsInfo = new TextView(this);
        fpsInfo.setTextSize(11);
        fpsInfo.setTextColor(0xFF999999);
        fpsInfo.setPadding(0, dp(12), 0, 0);
        box.addView(fpsInfo);

        // Resolution and fps lists depend on the chosen camera.
        final Runnable refreshModes = () -> {
            String id = cameraIds[Math.max(0, camSpin.getSelectedItemPosition())];
            List<Choice> res = new ArrayList<>();
            List<Choice> fpsList = new ArrayList<>();
            try {
                CameraCharacteristics ch = cm.getCameraCharacteristics(id);
                StreamConfigurationMap map = ch.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP);
                for (int[] r : RESOLUTIONS) {
                    if (!supportsSize(map, r[0], r[1])) continue;
                    String name = r[1] == 2160 ? "4K" : r[1] + "p";
                    res.add(new Choice(name + " (" + r[0] + "×" + r[1] + ")", r[0], r[1]));
                }
                // 60 fps: normal mode, or high-speed mode on the sizes that have it.
                boolean normal60 = fpsRangeFor(ch, 60) != null;
                StringBuilder hsSizes = new StringBuilder();
                for (int[] r : RESOLUTIONS) {
                    if (highSpeedRange(ch, map, new Size(r[0], r[1])) != null)
                        hsSizes.append(hsSizes.length() > 0 ? "/" : "").append(r[1]).append('p');
                }
                if (normal60) fpsList.add(new Choice("60 fps", 60, 0));
                else if (hsSizes.length() > 0)
                    fpsList.add(new Choice(Lang.t("60 fps (nagysebességű mód, ", "60 fps (high-speed mode, ") + hsSizes + ")", 60, 0));
                fpsList.add(new Choice("30 fps", 30, 0));
                fpsInfo.setText(describeFps(ch, map));
            } catch (Exception e) {
                fpsInfo.setText(Lang.t("Kamera infó hiba: ", "Camera info error: ") + e);
            }
            if (res.isEmpty()) res.add(new Choice("1080p (1920×1080)", 1920, 1080));
            if (fpsList.isEmpty()) fpsList.add(new Choice("30 fps", 30, 0));
            int rs = 0, fs = 0;
            for (int i = 0; i < res.size(); i++) if (res.get(i).a == prefs.width) rs = i;
            for (int i = 0; i < fpsList.size(); i++) if (fpsList.get(i).a == prefs.fps) fs = i;
            resSpin.setAdapter(new ArrayAdapter<>(this, android.R.layout.simple_spinner_dropdown_item, res));
            resSpin.setSelection(rs);
            fpsSpin.setAdapter(new ArrayAdapter<>(this, android.R.layout.simple_spinner_dropdown_item, fpsList));
            fpsSpin.setSelection(fs);
        };
        refreshModes.run();
        camSpin.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            @Override
            public void onItemSelected(AdapterView<?> parent, View view, int position, long id) {
                refreshModes.run();
            }

            @Override
            public void onNothingSelected(AdapterView<?> parent) {
            }
        });

        ScrollView scroll = new ScrollView(this);
        scroll.addView(box);

        new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle(Lang.t("Beállítások", "Settings"))
                .setView(scroll)
                .setNegativeButton(Lang.t("Mégse", "Cancel"), null)
                .setPositiveButton(Lang.t("Mentés", "Save"), (d, w) -> {
                    prefs.cameraId = cameraIds[Math.max(0, camSpin.getSelectedItemPosition())];
                    Choice r = (Choice) resSpin.getSelectedItem();
                    if (r != null) {
                        prefs.width = r.a;
                        prefs.height = r.b;
                    }
                    Choice f = (Choice) fpsSpin.getSelectedItem();
                    if (f != null) prefs.fps = f.a;
                    prefs.codec = ((Choice) codecSpin.getSelectedItem()).a;
                    prefs.bitrateMbps = ((Choice) rateSpin.getSelectedItem()).a;
                    prefs.stabilization = stab.isChecked();
                    prefs.manualIp = ipEdit.getText().toString().trim();
                    boolean english = langSpin.getSelectedItemPosition() == 1;
                    boolean languageChanged = english != Lang.english;
                    prefs.language = english ? "en" : "hu";
                    prefs.applyLanguage();
                    prefs.save();
                    net.setManualIp(prefs.manualIp);
                    if (languageChanged) {
                        recreate(); // rebuilds the screen in the new language
                        return;
                    }
                    restartStream();
                    hideSystemBars();
                })
                .show();
    }

    /** Keeps the preview at the stream's aspect ratio, fitted inside the screen. */
    private static final class AspectFrame extends FrameLayout {
        private double ratio = 16.0 / 9.0;

        AspectFrame(Context ctx) {
            super(ctx);
        }

        void setRatio(double r) {
            if (Math.abs(r - ratio) > 0.001) {
                ratio = r;
                requestLayout();
            }
        }

        @Override
        protected void onMeasure(int widthSpec, int heightSpec) {
            int w = MeasureSpec.getSize(widthSpec), h = MeasureSpec.getSize(heightSpec);
            if (w > h * ratio) w = (int) (h * ratio);
            else h = (int) (w / ratio);
            super.onMeasure(MeasureSpec.makeMeasureSpec(w, MeasureSpec.EXACTLY),
                    MeasureSpec.makeMeasureSpec(h, MeasureSpec.EXACTLY));
        }
    }
}
