package hu.phonecam;

import android.graphics.Rect;
import android.util.Range;

/** Fully resolved camera + encoder settings for one streaming session. */
final class StreamConfig {
    static final int CODEC_H264 = 0;
    static final int CODEC_HEVC = 1;

    String cameraId;
    int facing;
    int sensorOrientation;
    int width, height, fps;
    Range<Integer> fpsRange;
    /** Constrained high-speed session: the camera runs at captureFps, the encoder keeps fps. */
    boolean highSpeed;
    int captureFps;
    /** Why the stream differs from what was asked for, shown on screen. */
    String note;
    int codec;
    int bitrate; // bits per second
    int previewWidth, previewHeight;
    boolean realtimeTimestamps;
    boolean stabilization;
    boolean hasOis;
    boolean hasFlash;
    boolean continuousVideoAf;
    int maxAfRegions, maxAeRegions;
    Rect activeArray;
    float minZoom = 1f, maxZoom = 1f;
    /** Rotation the PC has to apply to get an upright image (0 or 180 in landscape). */
    volatile int rotation;

    String codecName() {
        return codec == CODEC_HEVC ? "H.265" : "H.264";
    }
}
