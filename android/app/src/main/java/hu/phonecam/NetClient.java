package hu.phonecam;

import android.content.Context;
import android.net.wifi.WifiManager;
import android.os.Build;
import android.os.SystemClock;
import android.provider.Settings;

import java.io.BufferedInputStream;
import java.io.BufferedOutputStream;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.IOException;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.net.SocketTimeoutException;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.LinkedBlockingDeque;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;

/**
 * Finds the PC (UDP beacons, USB via "adb reverse", or a manual IP), keeps a
 * TCP connection to it and streams encoded frames with a latency-first policy:
 * when the network falls behind, queued frames are dropped and the stream
 * resumes from a fresh key frame instead of building up delay.
 */
final class NetClient {
    static final int DISCOVERY_PORT = 47800;
    static final int STREAM_PORT = 47801;
    private static final int MAGIC = 0x5043414D; // "PCAM"
    private static final int VERSION = 1;

    // phone -> PC
    private static final int T_CONFIG = 1, T_FRAME = 2, T_PONG = 3, T_ORIENT = 4, T_INFO = 5;
    // PC -> phone
    private static final int P_REQUEST_KEY = 1, P_PING = 2, P_CAPS = 3, P_SET = 4;

    private static final int MAX_QUEUED_FRAMES = 4;

    interface Listener {
        void onNetStatus(String text, boolean connected);

        void onConnected(int codecMask);

        void onKeyFrameRequested();

        /** "key=value" lines sent by the PC app (camera, resolution, fps, torch, zoom, ...). */
        void onSettingsFromPc(String settings);
    }

    private static final class Packet {
        final int type;
        final byte[] data;
        final long pts;
        final boolean key;

        Packet(int type, byte[] data, long pts, boolean key) {
            this.type = type;
            this.data = data;
            this.pts = pts;
            this.key = key;
        }
    }

    private final class Connection {
        final Socket socket;
        final DataOutputStream out;
        final DataInputStream in;
        final LinkedBlockingDeque<Packet> queue = new LinkedBlockingDeque<>();
        final AtomicInteger queuedFrames = new AtomicInteger();
        volatile boolean alive = true;
        volatile boolean waitingForKey = true;

        Connection(Socket socket, DataOutputStream out, DataInputStream in) {
            this.socket = socket;
            this.out = out;
            this.in = in;
        }
    }

    private static final class Beacon {
        final String host, name;
        final int port;
        final long seenAt;

        Beacon(String host, int port, String name) {
            this.host = host;
            this.port = port;
            this.name = name;
            this.seenAt = SystemClock.elapsedRealtime();
        }
    }

    private final Context ctx;
    private final Listener listener;
    private volatile boolean running;
    private Thread connectThread, discoveryThread;
    private volatile DatagramSocket discoverySocket;
    private volatile Beacon beacon;
    private volatile String manualIp = "";
    private volatile Connection conn;
    private volatile StreamConfig config;
    private WifiManager.MulticastLock multicastLock;
    private WifiManager.WifiLock wifiLock;

    final AtomicInteger sentFrames = new AtomicInteger();
    final AtomicLong sentBytes = new AtomicLong();
    final AtomicInteger droppedFrames = new AtomicInteger();

    NetClient(Context ctx, Listener listener) {
        this.ctx = ctx.getApplicationContext();
        this.listener = listener;
    }

    void setManualIp(String ip) {
        manualIp = ip == null ? "" : ip.trim();
    }

    boolean isConnected() {
        Connection c = conn;
        return c != null && c.alive;
    }

    synchronized void start() {
        if (running) return;
        running = true;
        WifiManager wm = (WifiManager) ctx.getSystemService(Context.WIFI_SERVICE);
        if (wm != null) {
            multicastLock = wm.createMulticastLock("phonecam");
            multicastLock.setReferenceCounted(false);
            multicastLock.acquire();
            // Keeps Wi-Fi power saving from adding tens of ms of jitter.
            wifiLock = wm.createWifiLock(WifiManager.WIFI_MODE_FULL_LOW_LATENCY, "phonecam");
            wifiLock.setReferenceCounted(false);
            wifiLock.acquire();
        }
        discoveryThread = new Thread(this::discoveryLoop, "discovery");
        discoveryThread.start();
        connectThread = new Thread(this::connectLoop, "connect");
        connectThread.start();
    }

    synchronized void stop() {
        if (!running) return;
        running = false;
        DatagramSocket ds = discoverySocket;
        if (ds != null) ds.close();
        Connection c = conn;
        if (c != null) close(c, null);
        for (Thread t : new Thread[]{discoveryThread, connectThread}) {
            if (t == null) continue;
            t.interrupt();
            try {
                t.join(400);
            } catch (InterruptedException ignored) {
            }
        }
        if (multicastLock != null && multicastLock.isHeld()) multicastLock.release();
        if (wifiLock != null && wifiLock.isHeld()) wifiLock.release();
    }

    /** New stream parameters; the PC resets its decoder and waits for a key frame. */
    void setConfig(StreamConfig c) {
        config = c;
        Connection cn = conn;
        if (cn == null || !cn.alive) return;
        dropQueuedFrames(cn);
        cn.waitingForKey = true;
        cn.queue.offerFirst(new Packet(T_CONFIG, encodeConfig(c), 0, false));
    }

    /** Battery, current settings and camera capabilities for the PC window. */
    void sendInfo(String info) {
        Connection c = conn;
        if (c == null || !c.alive) return;
        c.queue.offerFirst(new Packet(T_INFO, info.getBytes(StandardCharsets.UTF_8), 0, false));
    }

    void sendOrientation(int rotation) {
        Connection c = conn;
        if (c == null || !c.alive) return;
        byte[] b = ByteBuffer.allocate(2).putShort((short) rotation).array();
        c.queue.offerFirst(new Packet(T_ORIENT, b, 0, false));
    }

    void sendFrame(byte[] data, long ptsUs, boolean key) {
        Connection c = conn;
        if (c == null || !c.alive) return;
        if (c.waitingForKey) {
            if (!key) return;
            c.waitingForKey = false;
        }
        if (c.queuedFrames.get() >= MAX_QUEUED_FRAMES) {
            // Network is behind: skip ahead instead of letting delay pile up.
            droppedFrames.addAndGet(dropQueuedFrames(c));
            if (!key) {
                c.waitingForKey = true;
                droppedFrames.incrementAndGet();
                listener.onKeyFrameRequested();
                return;
            }
        }
        c.queuedFrames.incrementAndGet();
        c.queue.offer(new Packet(T_FRAME, data, ptsUs, key));
    }

    private int dropQueuedFrames(Connection c) {
        int[] removed = {0};
        c.queue.removeIf(p -> {
            if (p.type != T_FRAME) return false;
            removed[0]++;
            return true;
        });
        c.queuedFrames.addAndGet(-removed[0]);
        return removed[0];
    }

    private static byte[] encodeConfig(StreamConfig c) {
        ByteBuffer b = ByteBuffer.allocate(20);
        b.put((byte) c.codec);
        b.putInt(c.width);
        b.putInt(c.height);
        b.putInt(c.fps);
        b.putInt(c.bitrate);
        b.putShort((short) c.rotation);
        b.put((byte) (c.realtimeTimestamps ? 1 : 0));
        return b.array();
    }

    /** Same clock as the camera timestamps, so the PC can measure end-to-end delay. */
    /** Clock of the frame timestamps, detected from the first frame (the camera's own report is unreliable). */
    static volatile boolean ptsMonotonic;

    private long phoneClockUs() {
        return ptsMonotonic ? System.nanoTime() / 1000 : SystemClock.elapsedRealtimeNanos() / 1000;
    }

    private String deviceName() {
        String name = Settings.Global.getString(ctx.getContentResolver(), Settings.Global.DEVICE_NAME);
        return name != null && !name.isEmpty() ? name : Build.MODEL;
    }

    // ------------------------------------------------------------------ threads

    private void discoveryLoop() {
        byte[] buf = new byte[512];
        while (running) {
            try (DatagramSocket ds = new DatagramSocket(null)) {
                discoverySocket = ds;
                ds.setReuseAddress(true);
                ds.setBroadcast(true);
                ds.bind(new InetSocketAddress(DISCOVERY_PORT));
                ds.setSoTimeout(1000);
                while (running) {
                    DatagramPacket p = new DatagramPacket(buf, buf.length);
                    try {
                        ds.receive(p);
                    } catch (SocketTimeoutException e) {
                        continue;
                    }
                    String msg = new String(buf, 0, p.getLength(), StandardCharsets.UTF_8);
                    if (!msg.startsWith("PHONECAM1 ")) continue;
                    String[] parts = msg.split(" ", 3);
                    try {
                        int port = Integer.parseInt(parts[1]);
                        String name = parts.length > 2 ? parts[2] : p.getAddress().getHostAddress();
                        beacon = new Beacon(p.getAddress().getHostAddress(), port, name);
                    } catch (NumberFormatException ignored) {
                    }
                }
            } catch (IOException e) {
                if (!running) return;
                SystemClock.sleep(1000);
            }
        }
    }

    private void connectLoop() {
        boolean announcedSearch = false;
        while (running) {
            Connection c = conn;
            if (c != null && c.alive) {
                SystemClock.sleep(300);
                continue;
            }
            // 1) USB cable with "adb reverse", 2) manual IP, 3) auto-discovered PC.
            if (tryConnect("127.0.0.1", STREAM_PORT, 300, true)) continue;
            String ip = manualIp;
            if (!ip.isEmpty() && tryConnect(ip, STREAM_PORT, 1500, false)) continue;
            Beacon b = beacon;
            if (b != null && SystemClock.elapsedRealtime() - b.seenAt < 5000 && tryConnect(b.host, b.port, 1500, false))
                continue;
            if (!announcedSearch || c != null) {
                listener.onNetStatus(Lang.t("Számítógép keresése… (indítsd el a PhoneCam-et a gépen)", "Looking for the PC… (start PhoneCam on the computer)"), false);
                announcedSearch = true;
            }
            SystemClock.sleep(700);
        }
    }

    private boolean tryConnect(String host, int port, int timeoutMs, boolean usb) {
        Socket s = new Socket();
        try {
            s.setTcpNoDelay(true);
            s.setSendBufferSize(512 * 1024);
            try {
                s.setTrafficClass(0xA0); // DSCP CS5 -> Wi-Fi video access category
            } catch (Exception ignored) {
            }
            s.connect(new InetSocketAddress(host, port), timeoutMs);
            DataOutputStream out = new DataOutputStream(new BufferedOutputStream(s.getOutputStream(), 64 * 1024));
            DataInputStream in = new DataInputStream(new BufferedInputStream(s.getInputStream(), 16 * 1024));
            out.writeInt(MAGIC);
            out.writeShort(VERSION);
            out.writeUTF(deviceName());
            out.writeByte(usb ? 1 : 0);
            out.flush();

            // The PC answers with its capabilities; anything else is not our PC.
            s.setSoTimeout(2000);
            int type = in.readUnsignedByte();
            int len = in.readInt();
            if (type != P_CAPS || len < 1 || len > 1024) throw new IOException("bad handshake");
            byte[] caps = new byte[len];
            in.readFully(caps);
            s.setSoTimeout(0);
            if (!running) throw new IOException("stopped");
            String pcName = len > 1 ? new String(caps, 1, len - 1, StandardCharsets.UTF_8) : host;

            Connection c = new Connection(s, out, in);
            conn = c;
            StreamConfig cfg = config;
            if (cfg != null) c.queue.offer(new Packet(T_CONFIG, encodeConfig(cfg), 0, false));
            new Thread(() -> writerLoop(c), "net-writer").start();
            new Thread(() -> readerLoop(c), "net-reader").start();
            listener.onNetStatus(Lang.t("Csatlakozva: ", "Connected: ") + pcName + (usb ? " (USB)" : " (Wi-Fi)"), true);
            listener.onConnected(caps[0] & 0xFF);
            listener.onKeyFrameRequested();
            return true;
        } catch (IOException e) {
            try {
                s.close();
            } catch (IOException ignored) {
            }
            return false;
        }
    }

    private void close(Connection c, String reason) {
        synchronized (c) {
            if (!c.alive) return;
            c.alive = false;
        }
        try {
            c.socket.close();
        } catch (IOException ignored) {
        }
        c.queue.clear();
        c.queue.offer(new Packet(0, new byte[0], 0, false)); // wake the writer
        if (conn == c) conn = null;
        if (reason != null && running) listener.onNetStatus(reason, false);
    }

    private void writerLoop(Connection c) {
        Thread.currentThread().setPriority(Thread.MAX_PRIORITY);
        try {
            while (c.alive) {
                Packet p = c.queue.poll(500, TimeUnit.MILLISECONDS);
                if (p == null || !c.alive) continue;
                if (p.type == T_FRAME) {
                    c.queuedFrames.decrementAndGet();
                    c.out.writeByte(T_FRAME);
                    c.out.writeInt(9 + p.data.length);
                    c.out.writeByte(p.key ? 1 : 0);
                    c.out.writeLong(p.pts);
                    c.out.write(p.data);
                    sentFrames.incrementAndGet();
                    sentBytes.addAndGet(p.data.length + 14);
                } else if (p.type != 0) {
                    c.out.writeByte(p.type);
                    c.out.writeInt(p.data.length);
                    c.out.write(p.data);
                }
                c.out.flush();
            }
        } catch (Exception e) {
            close(c, Lang.t("Kapcsolat megszakadt, újracsatlakozás…", "Connection lost, reconnecting…"));
        }
    }

    private void readerLoop(Connection c) {
        try {
            while (c.alive) {
                int type = c.in.readUnsignedByte();
                int len = c.in.readInt();
                if (len < 0 || len > 1 << 20) throw new IOException("bad packet");
                byte[] data = new byte[len];
                c.in.readFully(data);
                if (type == P_REQUEST_KEY) {
                    listener.onKeyFrameRequested();
                } else if (type == P_SET) {
                    listener.onSettingsFromPc(new String(data, StandardCharsets.UTF_8));
                } else if (type == P_PING && len >= 8) {
                    long pcTime = ByteBuffer.wrap(data).getLong();
                    byte[] pong = ByteBuffer.allocate(16).putLong(pcTime).putLong(phoneClockUs()).array();
                    c.queue.offerFirst(new Packet(T_PONG, pong, 0, false));
                }
            }
        } catch (Exception e) {
            close(c, Lang.t("Kapcsolat megszakadt, újracsatlakozás…", "Connection lost, reconnecting…"));
        }
    }
}
