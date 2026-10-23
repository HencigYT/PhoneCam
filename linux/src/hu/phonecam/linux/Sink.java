package hu.phonecam.linux;

import javax.imageio.ImageIO;
import java.awt.image.BufferedImage;
import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;
import java.util.function.Consumer;

/**
 * Feeds the virtual camera (a v4l2loopback device) through an ffmpeg process:
 * the phone's H.264/H.265 stream is decoded with low-delay flags, scaled to the
 * fixed output size and written to the device. Without a phone a dark
 * placeholder keeps the camera visible to apps.
 */
final class Sink {
    private final String ffmpeg;
    private volatile String output; // /dev/videoN, or a file / NUL for testing
    private Process process;
    private OutputStream stdin;
    private boolean placeholder;
    volatile String lastError = "";
    volatile int outWidth = 1920, outHeight = 1080;
    private final Consumer<BufferedImage> onPreview;
    private Boolean fpsModeOption;

    Sink(String ffmpeg, String output, Consumer<BufferedImage> onPreview) {
        this.ffmpeg = ffmpeg;
        this.output = output;
        this.onPreview = onPreview;
    }

    String output() {
        return output;
    }

    void setOutput(String output) {
        this.output = output;
    }

    /** v4l2loopback device named "PhoneCam" (see --setup), or any loopback device. */
    static String findDevice() {
        File[] devs = new File("/sys/class/video4linux").listFiles();
        if (devs == null) return null;
        String fallback = null;
        for (File d : devs) {
            try {
                String name = new String(Files.readAllBytes(new File(d, "name").toPath()), StandardCharsets.UTF_8).trim();
                if (name.contains("PhoneCam")) return "/dev/" + d.getName();
                if (name.toLowerCase().contains("dummy") || name.toLowerCase().contains("loopback"))
                    fallback = "/dev/" + d.getName();
            } catch (IOException ignored) {
            }
        }
        return fallback;
    }

    boolean available() {
        try {
            Process p = new ProcessBuilder(ffmpeg, "-hide_banner", "-version").redirectErrorStream(true).start();
            String text = new String(p.getInputStream().readAllBytes(), StandardCharsets.UTF_8);
            p.waitFor();
            // -fps_mode exists since ffmpeg 5.1; older ones only know -vsync.
            java.util.regex.Matcher m = java.util.regex.Pattern.compile("version n?(\\d+)\\.(\\d+)").matcher(text);
            fpsModeOption = !m.find() || Integer.parseInt(m.group(1)) * 100 + Integer.parseInt(m.group(2)) >= 501;
            return true;
        } catch (Exception e) {
            return false;
        }
    }

    private List<String> outputArgs() {
        List<String> a = new ArrayList<>();
        if (output.startsWith("/dev/")) {
            a.add("-f");
            a.add("v4l2");
        } else {
            a.add("-f");
            a.add("rawvideo");
        }
        a.add(output);
        return a;
    }

    /** Starts decoding a new stream; codec 0 = H.264, 1 = H.265. */
    synchronized void startStream(int codec, boolean hflip, boolean vflip, boolean preview) {
        stop();
        if (output == null) return;
        int w = outWidth, h = outHeight;
        StringBuilder vf = new StringBuilder("[0:v]scale=" + w + ":" + h
                + ":force_original_aspect_ratio=decrease:flags=fast_bilinear,pad=" + w + ":" + h
                + ":(ow-iw)/2:(oh-ih)/2");
        if (hflip) vf.append(",hflip");
        if (vflip) vf.append(",vflip");
        vf.append(",format=yuv420p");
        if (preview) vf.append(",split=2[o][p0];[p0]fps=10,scale=480:-2[p]");
        else vf.append("[o]");

        List<String> cmd = new ArrayList<>(List.of(ffmpeg, "-hide_banner", "-loglevel", "error", "-y",
                "-fflags", "nobuffer", "-flags", "low_delay", "-probesize", "32", "-analyzeduration", "0",
                "-f", codec == 1 ? "hevc" : "h264", "-i", "pipe:0", "-filter_complex", vf.toString(),
                "-map", "[o]"));
        cmd.add(Boolean.FALSE.equals(fpsModeOption) ? "-vsync" : "-fps_mode");
        cmd.add("passthrough");
        cmd.addAll(outputArgs());
        if (preview) {
            cmd.addAll(List.of("-map", "[p]", "-f", "image2pipe", "-c:v", "mjpeg", "-q:v", "6", "pipe:1"));
        }
        launch(cmd, preview);
        placeholder = false;
    }

    /** Dark frames so the camera stays visible to apps while no phone is connected. */
    synchronized void startPlaceholder() {
        if (placeholder && process != null && process.isAlive()) return;
        stop();
        if (output == null) return;
        List<String> cmd = new ArrayList<>(List.of(ffmpeg, "-hide_banner", "-loglevel", "error", "-y", "-re",
                "-f", "lavfi", "-i", "color=c=0x111318:s=" + outWidth + "x" + outHeight + ":r=5",
                "-vf", "format=yuv420p"));
        cmd.addAll(outputArgs());
        launch(cmd, false);
        placeholder = true;
    }

    private void launch(List<String> cmd, boolean preview) {
        try {
            ProcessBuilder pb = new ProcessBuilder(cmd);
            if (!preview) pb.redirectOutput(ProcessBuilder.Redirect.DISCARD);
            process = pb.start();
            stdin = process.getOutputStream();
            final Process p = process;
            Thread err = new Thread(() -> {
                try (InputStream es = p.getErrorStream()) {
                    byte[] buf = new byte[4096];
                    int n;
                    while ((n = es.read(buf)) > 0) {
                        String s = new String(buf, 0, n, StandardCharsets.UTF_8).trim();
                        if (!s.isEmpty()) lastError = s.lines().reduce((a, b) -> b).orElse(s);
                    }
                } catch (IOException ignored) {
                }
            }, "ffmpeg-log");
            err.setDaemon(true);
            err.start();
            if (preview) {
                Thread prev = new Thread(() -> readPreview(p), "ffmpeg-preview");
                prev.setDaemon(true);
                prev.start();
            }
            lastError = "";
        } catch (IOException e) {
            lastError = Lang.t("Az ffmpeg nem indul: ", "ffmpeg does not start: ") + e.getMessage();
            process = null;
            stdin = null;
        }
    }

    /** Splits the MJPEG pipe into JPEG images (SOI 0xFFD8 ... EOI 0xFFD9). */
    private void readPreview(Process p) {
        try (InputStream in = p.getInputStream()) {
            ByteArrayOutputStream jpeg = new ByteArrayOutputStream(64 * 1024);
            int prev = -1, b;
            boolean inImage = false;
            while ((b = in.read()) >= 0) {
                if (!inImage) {
                    if (prev == 0xFF && b == 0xD8) {
                        inImage = true;
                        jpeg.reset();
                        jpeg.write(0xFF);
                        jpeg.write(0xD8);
                    }
                } else {
                    jpeg.write(b);
                    if (prev == 0xFF && b == 0xD9) {
                        inImage = false;
                        BufferedImage img = ImageIO.read(new ByteArrayInputStream(jpeg.toByteArray()));
                        if (img != null && onPreview != null) onPreview.accept(img);
                    }
                }
                prev = b;
            }
        } catch (IOException ignored) {
        }
    }

    /** Writes one encoded frame; false if ffmpeg is gone. */
    boolean write(byte[] data, int off, int len) {
        OutputStream s;
        synchronized (this) {
            s = stdin;
        }
        if (s == null) return false;
        try {
            s.write(data, off, len);
            s.flush();
            return true;
        } catch (IOException e) {
            return false;
        }
    }

    synchronized boolean alive() {
        return process != null && process.isAlive();
    }

    synchronized void stop() {
        if (process == null) return;
        try {
            stdin.close();
        } catch (IOException ignored) {
        }
        process.destroy();
        try {
            if (!process.waitFor(1, java.util.concurrent.TimeUnit.SECONDS)) process.destroyForcibly();
        } catch (InterruptedException ignored) {
        }
        process = null;
        stdin = null;
    }
}
