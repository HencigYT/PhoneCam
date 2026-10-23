package hu.phonecam;

import android.annotation.SuppressLint;
import android.content.Context;
import android.graphics.Rect;
import android.hardware.camera2.CameraAccessException;
import android.hardware.camera2.CameraCaptureSession;
import android.hardware.camera2.CameraConstrainedHighSpeedCaptureSession;
import android.hardware.camera2.CameraDevice;
import android.hardware.camera2.CameraManager;
import android.hardware.camera2.CaptureRequest;
import android.hardware.camera2.params.MeteringRectangle;
import android.hardware.camera2.params.OutputConfiguration;
import android.hardware.camera2.params.SessionConfiguration;
import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaCodecList;
import android.media.MediaFormat;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;
import android.util.Log;
import android.view.Surface;

import java.nio.ByteBuffer;
import java.util.Arrays;
import java.util.concurrent.Executor;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * Camera2 -> hardware encoder pipeline. The camera renders straight into the
 * encoder's input surface (zero copy); encoded frames go to the listener.
 */
final class CameraStreamer {
    private static final String TAG = "PhoneCam";

    interface Listener {
        void onEncodedFrame(byte[] data, long ptsUs, boolean keyFrame);

        void onStreamerError(String message);
    }

    private final Context ctx;
    private final Listener listener;

    private HandlerThread camThread;
    private Handler camHandler;
    private CameraDevice camera;
    private CameraCaptureSession session;
    private CaptureRequest.Builder request;
    private MediaCodec codec;
    private Surface encoderSurface;
    private Surface previewSurface;
    private Thread drainThread;
    private volatile boolean draining;
    private StreamConfig cfg;
    private int generation;

    private float zoom = 1f;
    private boolean torch;
    private final Runnable resetFocus = this::resetFocus;

    final AtomicInteger encodedFrames = new AtomicInteger();

    CameraStreamer(Context ctx, Listener listener) {
        this.ctx = ctx.getApplicationContext();
        this.listener = listener;
    }

    synchronized boolean isRunning() {
        return cfg != null;
    }

    synchronized void start(Surface preview, StreamConfig config) {
        stop();
        cfg = config;
        previewSurface = preview;
        final int gen = ++generation;
        zoom = Math.max(config.minZoom, Math.min(config.maxZoom, zoom));
        if (!config.hasFlash) torch = false;

        camThread = new HandlerThread("camera");
        camThread.start();
        camHandler = new Handler(camThread.getLooper());

        try {
            setupEncoder(config);
        } catch (Exception e) {
            Log.e(TAG, "encoder", e);
            cfg = null;
            listener.onStreamerError(Lang.t("Kódoló hiba: ", "Encoder error: ") + e.getMessage());
            return;
        }
        startDrain();

        CameraManager cm = (CameraManager) ctx.getSystemService(Context.CAMERA_SERVICE);
        try {
            openCamera(cm, config.cameraId, gen);
        } catch (Exception e) {
            Log.e(TAG, "openCamera", e);
            listener.onStreamerError(Lang.t("Kamera hiba: ", "Camera error: ") + e.getMessage());
        }
    }

    synchronized void stop() {
        generation++;
        if (camHandler != null) camHandler.removeCallbacksAndMessages(null);
        try {
            if (session != null) session.close();
        } catch (Exception ignored) {
        }
        session = null;
        request = null;
        if (camera != null) camera.close();
        camera = null;

        draining = false;
        if (drainThread != null) {
            try {
                drainThread.join(1000);
            } catch (InterruptedException ignored) {
            }
            drainThread = null;
        }
        if (codec != null) {
            try {
                codec.stop();
            } catch (Exception ignored) {
            }
            codec.release();
            codec = null;
        }
        if (encoderSurface != null) encoderSurface.release();
        encoderSurface = null;
        if (camThread != null) camThread.quitSafely();
        camThread = null;
        camHandler = null;
        cfg = null;
    }

    void requestKeyFrame() {
        MediaCodec c = codec;
        if (c == null) return;
        try {
            Bundle b = new Bundle();
            b.putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0);
            c.setParameters(b);
        } catch (Exception ignored) {
        }
    }

    synchronized float getZoom() {
        return zoom;
    }

    synchronized void setZoom(float value) {
        zoom = value;
        if (cfg != null) zoom = Math.max(cfg.minZoom, Math.min(cfg.maxZoom, value));
        postUpdate();
    }

    synchronized boolean toggleTorch() {
        return setTorch(!torch);
    }

    synchronized boolean setTorch(boolean on) {
        if (cfg == null || !cfg.hasFlash) return false;
        torch = on;
        postUpdate();
        return torch;
    }

    synchronized boolean isTorch() {
        return torch;
    }

    /** Tap to focus; x/y are normalized coordinates in the sensor's orientation. */
    synchronized void focusAt(float x, float y) {
        final StreamConfig c = cfg;
        final Handler h = camHandler;
        if (c == null || h == null || c.activeArray == null || (c.maxAfRegions == 0 && c.maxAeRegions == 0)) return;
        h.post(() -> {
            synchronized (CameraStreamer.this) {
                if (session == null || request == null) return;
                Rect a = c.activeArray;
                float z = Math.max(zoom, 1f);
                float cw = a.width() / z, ch = a.height() / z;
                float left = a.centerX() - cw / 2, top = a.centerY() - ch / 2;
                int half = (int) (Math.min(cw, ch) * 0.08f);
                int px = (int) (left + x * cw), py = (int) (top + y * ch);
                int l = Math.max(a.left, px - half), t = Math.max(a.top, py - half);
                int r = Math.min(a.right - 1, px + half), b = Math.min(a.bottom - 1, py + half);
                MeteringRectangle[] region = {new MeteringRectangle(new Rect(l, t, r, b),
                        MeteringRectangle.METERING_WEIGHT_MAX - 1)};
                try {
                    if (c.maxAfRegions > 0) request.set(CaptureRequest.CONTROL_AF_REGIONS, region);
                    if (c.maxAeRegions > 0) request.set(CaptureRequest.CONTROL_AE_REGIONS, region);
                    request.set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_AUTO);
                    request.set(CaptureRequest.CONTROL_AF_TRIGGER, CaptureRequest.CONTROL_AF_TRIGGER_START);
                    captureOnce();
                    request.set(CaptureRequest.CONTROL_AF_TRIGGER, CaptureRequest.CONTROL_AF_TRIGGER_IDLE);
                    repeat();
                } catch (Exception e) {
                    Log.w(TAG, "focus", e);
                }
                camHandler.removeCallbacks(resetFocus);
                camHandler.postDelayed(resetFocus, 5000);
            }
        });
    }

    private synchronized void resetFocus() {
        if (session == null || request == null || cfg == null) return;
        MeteringRectangle[] none = {new MeteringRectangle(0, 0, 0, 0, 0)};
        try {
            if (cfg.maxAfRegions > 0) request.set(CaptureRequest.CONTROL_AF_REGIONS, none);
            if (cfg.maxAeRegions > 0) request.set(CaptureRequest.CONTROL_AE_REGIONS, none);
            request.set(CaptureRequest.CONTROL_AF_TRIGGER, CaptureRequest.CONTROL_AF_TRIGGER_CANCEL);
            captureOnce();
            request.set(CaptureRequest.CONTROL_AF_TRIGGER, CaptureRequest.CONTROL_AF_TRIGGER_IDLE);
            applyAfMode();
            repeat();
        } catch (Exception e) {
            Log.w(TAG, "resetFocus", e);
        }
    }

    // ------------------------------------------------------------------ encoder

    private static String mimeOf(int codec) {
        return codec == StreamConfig.CODEC_HEVC ? MediaFormat.MIMETYPE_VIDEO_HEVC : MediaFormat.MIMETYPE_VIDEO_AVC;
    }

    /** Hardware encoder able to do the size/rate, or null. */
    static MediaCodecInfo findEncoder(int codec, int w, int h, int fps) {
        String mime = mimeOf(codec);
        MediaCodecInfo fallback = null;
        for (MediaCodecInfo info : new MediaCodecList(MediaCodecList.REGULAR_CODECS).getCodecInfos()) {
            if (!info.isEncoder() || !Arrays.asList(info.getSupportedTypes()).contains(mime)) continue;
            MediaCodecInfo.VideoCapabilities vc = info.getCapabilitiesForType(mime).getVideoCapabilities();
            if (vc == null || !vc.areSizeAndRateSupported(w, h, fps)) continue;
            if (info.isHardwareAccelerated()) return info;
            if (fallback == null) fallback = info;
        }
        return fallback;
    }

    private void setupEncoder(StreamConfig c) throws Exception {
        String mime = mimeOf(c.codec);
        MediaCodecInfo info = findEncoder(c.codec, c.width, c.height, c.fps);
        if (info == null) throw new IllegalStateException(Lang.t("nincs " + c.codecName() + " kódoló ehhez a módhoz", "no " + c.codecName() + " encoder for this mode"));
        MediaCodecInfo.CodecCapabilities caps = info.getCapabilitiesForType(mime);

        MediaFormat base = MediaFormat.createVideoFormat(mime, c.width, c.height);
        base.setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface);
        base.setInteger(MediaFormat.KEY_BIT_RATE, c.bitrate);
        base.setInteger(MediaFormat.KEY_FRAME_RATE, c.fps);
        base.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 2);
        // High-speed mode: the camera delivers e.g. 120 fps, the encoder keeps every 2nd frame.
        if (c.captureFps > c.fps) base.setFloat(MediaFormat.KEY_MAX_FPS_TO_ENCODER, c.fps);
        if (caps.getEncoderCapabilities().isBitrateModeSupported(
                MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR)) {
            base.setInteger(MediaFormat.KEY_BITRATE_MODE, MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR);
        }
        if (c.codec == StreamConfig.CODEC_H264) {
            for (MediaCodecInfo.CodecProfileLevel pl : caps.profileLevels) {
                if (pl.profile == MediaCodecInfo.CodecProfileLevel.AVCProfileHigh) {
                    base.setInteger(MediaFormat.KEY_PROFILE, MediaCodecInfo.CodecProfileLevel.AVCProfileHigh);
                    break;
                }
            }
        } else {
            base.setInteger(MediaFormat.KEY_PROFILE, MediaCodecInfo.CodecProfileLevel.HEVCProfileMain);
        }
        base.setInteger(MediaFormat.KEY_COLOR_STANDARD, MediaFormat.COLOR_STANDARD_BT709);
        base.setInteger(MediaFormat.KEY_COLOR_RANGE, MediaFormat.COLOR_RANGE_LIMITED);
        base.setInteger(MediaFormat.KEY_COLOR_TRANSFER, MediaFormat.COLOR_TRANSFER_SDR_VIDEO);

        // Low latency extras; dropped again if the encoder refuses them.
        MediaFormat tuned = new MediaFormat(base);
        tuned.setInteger(MediaFormat.KEY_PRIORITY, 0);
        tuned.setInteger(MediaFormat.KEY_MAX_B_FRAMES, 0);
        tuned.setInteger(MediaFormat.KEY_OPERATING_RATE, Math.max(c.fps, c.captureFps));
        tuned.setInteger(MediaFormat.KEY_LATENCY, 1);
        tuned.setInteger("vendor.qti-ext-enc-low-latency.enable", 1);

        codec = MediaCodec.createByCodecName(info.getName());
        try {
            codec.configure(tuned, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
        } catch (Exception e) {
            Log.w(TAG, "tuned encoder config rejected, using base", e);
            codec.reset();
            codec.configure(base, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
        }
        encoderSurface = codec.createInputSurface();
        codec.start();
    }

    private void startDrain() {
        final MediaCodec c = codec;
        draining = true;
        drainThread = new Thread(() -> {
            MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
            byte[] csd = null;
            boolean clockChecked = false;
            while (draining) {
                int index;
                try {
                    index = c.dequeueOutputBuffer(info, 20000);
                } catch (Exception e) {
                    break;
                }
                if (index < 0) continue;
                try {
                    ByteBuffer buf = c.getOutputBuffer(index);
                    if (buf != null && info.size > 0) {
                        buf.position(info.offset);
                        buf.limit(info.offset + info.size);
                        if ((info.flags & MediaCodec.BUFFER_FLAG_CODEC_CONFIG) != 0) {
                            csd = new byte[info.size];
                            buf.get(csd);
                        } else {
                            boolean key = (info.flags & MediaCodec.BUFFER_FLAG_KEY_FRAME) != 0;
                            if (!clockChecked) {
                                // Frame times are either boot time or monotonic time; pick the closer one.
                                long boot = android.os.SystemClock.elapsedRealtimeNanos() / 1000;
                                long mono = System.nanoTime() / 1000;
                                NetClient.ptsMonotonic = Math.abs(mono - info.presentationTimeUs)
                                        < Math.abs(boot - info.presentationTimeUs);
                                clockChecked = true;
                            }
                            // Key frames carry the parameter sets so the PC can start anywhere.
                            int prefix = key && csd != null ? csd.length : 0;
                            byte[] out = new byte[prefix + info.size];
                            if (prefix > 0) System.arraycopy(csd, 0, out, 0, prefix);
                            buf.get(out, prefix, info.size);
                            encodedFrames.incrementAndGet();
                            listener.onEncodedFrame(out, info.presentationTimeUs, key);
                        }
                    }
                    c.releaseOutputBuffer(index, false);
                } catch (Exception e) {
                    Log.w(TAG, "drain", e);
                    break;
                }
            }
        }, "encoder-drain");
        drainThread.setPriority(Thread.MAX_PRIORITY);
        drainThread.start();
    }

    // ------------------------------------------------------------------ camera

    @SuppressLint("MissingPermission")
    private void openCamera(CameraManager cm, String id, final int gen) throws CameraAccessException {
        cm.openCamera(id, new CameraDevice.StateCallback() {
            @Override
            public void onOpened(CameraDevice device) {
                synchronized (CameraStreamer.this) {
                    if (gen != generation) {
                        device.close();
                        return;
                    }
                    camera = device;
                    createSession(gen);
                }
            }

            @Override
            public void onDisconnected(CameraDevice device) {
                device.close();
                synchronized (CameraStreamer.this) {
                    if (gen == generation) {
                        camera = null;
                        listener.onStreamerError(Lang.t("A kamerát egy másik alkalmazás elvette", "Another app took the camera"));
                    }
                }
            }

            @Override
            public void onError(CameraDevice device, int error) {
                device.close();
                synchronized (CameraStreamer.this) {
                    if (gen == generation) {
                        camera = null;
                        listener.onStreamerError(Lang.t("Kamera hiba (", "Camera error (") + error + ")");
                    }
                }
            }
        }, camHandler);
    }

    private void createSession(final int gen) {
        final Handler h = camHandler;
        Executor executor = h::post;
        SessionConfiguration sc = new SessionConfiguration(cfg.highSpeed ? SessionConfiguration.SESSION_HIGH_SPEED : SessionConfiguration.SESSION_REGULAR,
                Arrays.asList(new OutputConfiguration(previewSurface), new OutputConfiguration(encoderSurface)),
                executor, new CameraCaptureSession.StateCallback() {
            @Override
            public void onConfigured(CameraCaptureSession s) {
                synchronized (CameraStreamer.this) {
                    if (gen != generation) {
                        s.close();
                        return;
                    }
                    session = s;
                    try {
                        buildRequest();
                        repeat();
                    } catch (Exception e) {
                        Log.e(TAG, "repeating", e);
                        listener.onStreamerError(Lang.t("Kamera indítási hiba: ", "Camera start error: ") + e.getMessage());
                    }
                }
            }

            @Override
            public void onConfigureFailed(CameraCaptureSession s) {
                synchronized (CameraStreamer.this) {
                    if (gen == generation)
                        listener.onStreamerError(Lang.t("Ezt a felbontás/fps párost a kamera nem támogatja", "The camera does not support this resolution/fps"));
                }
            }
        });
        try {
            camera.createCaptureSession(sc);
        } catch (Exception e) {
            Log.e(TAG, "session", e);
            listener.onStreamerError(Lang.t("Kamera session hiba: ", "Camera session error: ") + e.getMessage());
        }
    }

    private void buildRequest() throws CameraAccessException {
        request = camera.createCaptureRequest(CameraDevice.TEMPLATE_RECORD);
        request.addTarget(previewSurface);
        request.addTarget(encoderSurface);
        request.set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, cfg.fpsRange);
        if (!cfg.highSpeed) {
            // Electronic stabilization buffers frames (= extra delay), so it is opt-in.
            request.set(CaptureRequest.CONTROL_VIDEO_STABILIZATION_MODE, cfg.stabilization
                    ? CaptureRequest.CONTROL_VIDEO_STABILIZATION_MODE_ON
                    : CaptureRequest.CONTROL_VIDEO_STABILIZATION_MODE_OFF);
            if (cfg.hasOis)
                request.set(CaptureRequest.LENS_OPTICAL_STABILIZATION_MODE,
                        CaptureRequest.LENS_OPTICAL_STABILIZATION_MODE_ON);
        }
        applyAfMode();
        applyDynamic();
    }

    // High-speed sessions only accept bursts built by createHighSpeedRequestList.
    private void repeat() throws CameraAccessException {
        if (session instanceof CameraConstrainedHighSpeedCaptureSession) {
            CameraConstrainedHighSpeedCaptureSession hs = (CameraConstrainedHighSpeedCaptureSession) session;
            hs.setRepeatingBurst(hs.createHighSpeedRequestList(request.build()), null, camHandler);
        } else {
            session.setRepeatingRequest(request.build(), null, camHandler);
        }
    }

    private void captureOnce() throws CameraAccessException {
        if (session instanceof CameraConstrainedHighSpeedCaptureSession) {
            CameraConstrainedHighSpeedCaptureSession hs = (CameraConstrainedHighSpeedCaptureSession) session;
            hs.captureBurst(hs.createHighSpeedRequestList(request.build()), null, camHandler);
        } else {
            session.capture(request.build(), null, camHandler);
        }
    }

    private void applyAfMode() {
        if (cfg.continuousVideoAf)
            request.set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_VIDEO);
    }

    private void applyDynamic() {
        if (cfg.maxZoom > cfg.minZoom) request.set(CaptureRequest.CONTROL_ZOOM_RATIO, zoom);
        if (cfg.hasFlash)
            request.set(CaptureRequest.FLASH_MODE,
                    torch ? CaptureRequest.FLASH_MODE_TORCH : CaptureRequest.FLASH_MODE_OFF);
    }

    private void postUpdate() {
        Handler h = camHandler;
        if (h == null) return;
        h.post(() -> {
            synchronized (CameraStreamer.this) {
                if (session == null || request == null) return;
                try {
                    applyDynamic();
                    repeat();
                } catch (Exception e) {
                    Log.w(TAG, "update", e);
                }
            }
        });
    }
}
