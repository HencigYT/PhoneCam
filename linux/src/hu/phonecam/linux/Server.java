package hu.phonecam.linux;

import java.io.BufferedInputStream;
import java.io.BufferedOutputStream;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.IOException;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.InterfaceAddress;
import java.net.NetworkInterface;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/**
 * Receives the phone stream (same protocol as the Windows app): UDP beacons
 * advertise the PC, the phone connects over TCP and sends config, frames, pongs
 * and info; the PC sends pings, key frame requests and settings.
 */
final class Server {
    static final int DISCOVERY_PORT = 47800;
    static final int STREAM_PORT = Integer.getInteger("phonecam.port", 47801);
    private static final int MAGIC = 0x5043414D;
    // phone -> PC
    private static final int T_CONFIG = 1, T_FRAME = 2, T_PONG = 3, T_ORIENT = 4, T_INFO = 5;
    // PC -> phone
    private static final int P_REQUEST_KEY = 1, P_PING = 2, P_CAPS = 3, P_SET = 4;

    private final Sink sink;
    private volatile boolean running;
    private ServerSocket listen;
    private volatile Socket client;
    private final Object sendLock = new Object();
    private volatile DataOutputStream out;

    // state shown in the window
    volatile boolean connected, usb;
    volatile String phoneName = "", phoneAddress = "";
    volatile int codec, width, height, fps, bitrate, rotation;
    volatile double inFps, inMbps, rttMs = -1;
    volatile PhoneInfo info;
    volatile long infoSerial;
    volatile List<String> addresses = Collections.emptyList();
    volatile boolean mirror, flip, preview = true, usbMode = true;
    volatile boolean beacons = true;

    Server(Sink sink) {
        this.sink = sink;
    }

    void start() throws IOException {
        listen = new ServerSocket();
        listen.setReuseAddress(false);
        listen.bind(new InetSocketAddress(STREAM_PORT));
        running = true;
        daemon(this::acceptLoop, "accept");
        if (beacons) {
            daemon(this::beaconLoop, "beacon");
            daemon(this::adbLoop, "adb");
        }
        sink.startPlaceholder();
    }

    void stop() {
        running = false;
        try {
            listen.close();
        } catch (IOException ignored) {
        }
        Socket c = client;
        if (c != null) try {
            c.close();
        } catch (IOException ignored) {
        }
        sink.stop();
    }

    private static void daemon(Runnable r, String name) {
        Thread t = new Thread(r, name);
        t.setDaemon(true);
        t.start();
    }

    /** Restarts the decoder so mirror / rotation changes take effect. */
    void restartVideo() {
        if (connected && width > 0) {
            boolean rotate = (rotation == 180) != flip;
            sink.startStream(codec, rotate != mirror, rotate, preview);
            send(P_REQUEST_KEY, new byte[0]);
        } else {
            sink.startPlaceholder();
        }
    }

    boolean sendSettings(String settings) {
        return connected && send(P_SET, settings.getBytes(StandardCharsets.UTF_8));
    }

    private boolean send(int type, byte[] payload) {
        DataOutputStream o = out;
        if (o == null) return false;
        synchronized (sendLock) {
            try {
                o.writeByte(type);
                o.writeInt(payload.length);
                o.write(payload);
                o.flush();
                return true;
            } catch (IOException e) {
                return false;
            }
        }
    }

    private void acceptLoop() {
        while (running) {
            try {
                Socket s = listen.accept();
                Socket old = client;
                if (old != null) old.close(); // the newest connection wins (reconnects)
                client = s;
                daemon(() -> clientLoop(s), "client");
            } catch (IOException e) {
                if (!running) return;
            }
        }
    }

    private static String hostName() {
        try {
            return InetAddress.getLocalHost().getHostName();
        } catch (IOException e) {
            return "Linux PC";
        }
    }

    private void clientLoop(Socket s) {
        long lastKeyRequest = 0;
        try {
            s.setTcpNoDelay(true);
            s.setReceiveBufferSize(4 << 20);
            s.setSoTimeout(5000); // the phone talks constantly; silence means it is gone
            DataInputStream in = new DataInputStream(new BufferedInputStream(s.getInputStream(), 1 << 16));
            DataOutputStream o = new DataOutputStream(new BufferedOutputStream(s.getOutputStream(), 1 << 12));
            if (in.readInt() != MAGIC) throw new IOException("bad hello");
            in.readUnsignedShort();
            String name = in.readUTF();
            boolean viaUsb = in.readUnsignedByte() == 1;
            out = o;
            byte[] host = hostName().getBytes(StandardCharsets.UTF_8);
            byte[] caps = new byte[1 + host.length];
            caps[0] = 3; // ffmpeg decodes H.264 and H.265
            System.arraycopy(host, 0, caps, 1, host.length);
            send(P_CAPS, caps);

            phoneName = name;
            usb = viaUsb;
            phoneAddress = viaUsb ? "USB" : s.getInetAddress().getHostAddress();
            width = 0;
            info = null;
            infoSerial++;
            connected = true;

            daemon(() -> {
                while (connected && client == s) {
                    send(P_PING, ByteBuffer.allocate(8).putLong(System.nanoTime() / 1000).array());
                    try {
                        Thread.sleep(1000);
                    } catch (InterruptedException e) {
                        return;
                    }
                }
            }, "ping");

            boolean needKey = true;
            int frames = 0;
            long bytes = 0, statsAt = System.nanoTime();
            byte[] buf = new byte[1 << 20];
            while (running) {
                int type = in.readUnsignedByte();
                int len = in.readInt();
                if (len < 0 || len > (64 << 20)) throw new IOException("bad packet");
                if (buf.length < len) buf = new byte[len];
                in.readFully(buf, 0, len);
                ByteBuffer p = ByteBuffer.wrap(buf, 0, len);
                switch (type) {
                    case T_CONFIG:
                        if (len < 20) break;
                        codec = p.get() & 0xFF;
                        width = p.getInt();
                        height = p.getInt();
                        fps = p.getInt();
                        bitrate = p.getInt();
                        rotation = p.getShort() & 0xFFFF;
                        restartVideo();
                        needKey = true;
                        lastKeyRequest = 0;
                        break;
                    case T_FRAME: {
                        if (len < 9) break;
                        boolean key = (buf[0] & 1) != 0;
                        frames++;
                        bytes += len;
                        if (needKey && !key) {
                            long now = System.nanoTime();
                            if (now - lastKeyRequest > 300_000_000L) {
                                lastKeyRequest = now;
                                send(P_REQUEST_KEY, new byte[0]);
                            }
                            break;
                        }
                        needKey = false;
                        if (!sink.write(buf, 9, len - 9)) {
                            restartVideo(); // ffmpeg died: start over from the next key frame
                            needKey = true;
                        }
                        break;
                    }
                    case T_PONG:
                        if (len >= 16) rttMs = (System.nanoTime() / 1000 - p.getLong()) / 1000.0;
                        break;
                    case T_ORIENT:
                        if (len >= 2) {
                            rotation = p.getShort() & 0xFFFF;
                            restartVideo();
                            needKey = true;
                        }
                        break;
                    case T_INFO:
                        info = PhoneInfo.parse(new String(buf, 0, len, StandardCharsets.UTF_8));
                        infoSerial++;
                        break;
                    default:
                        break;
                }
                long now = System.nanoTime();
                if (now - statsAt >= 1_000_000_000L) {
                    double secs = (now - statsAt) / 1e9;
                    inFps = frames / secs;
                    inMbps = bytes * 8 / secs / 1e6;
                    frames = 0;
                    bytes = 0;
                    statsAt = now;
                }
            }
        } catch (IOException ignored) {
        } finally {
            try {
                s.close();
            } catch (IOException ignored) {
            }
            if (client == s) {
                connected = false;
                out = null;
                info = null;
                infoSerial++;
                inFps = inMbps = 0;
                rttMs = -1;
                sink.startPlaceholder();
            }
        }
    }

    private void beaconLoop() {
        byte[] msg = ("PHONECAM1 " + STREAM_PORT + " " + hostName()).getBytes(StandardCharsets.UTF_8);
        try (DatagramSocket ds = new DatagramSocket()) {
            ds.setBroadcast(true);
            while (running) {
                List<InetAddress> targets = new ArrayList<>();
                List<String> addrs = new ArrayList<>();
                try {
                    targets.add(InetAddress.getByName("255.255.255.255"));
                    for (NetworkInterface ni : Collections.list(NetworkInterface.getNetworkInterfaces())) {
                        if (!ni.isUp() || ni.isLoopback()) continue;
                        for (InterfaceAddress ia : ni.getInterfaceAddresses()) {
                            if (ia.getBroadcast() == null) continue;
                            targets.add(ia.getBroadcast());
                            addrs.add(ia.getAddress().getHostAddress() + " (" + ni.getName() + ")");
                        }
                    }
                } catch (IOException ignored) {
                }
                addresses = addrs;
                for (InetAddress t : targets) {
                    try {
                        ds.send(new DatagramPacket(msg, msg.length, t, DISCOVERY_PORT));
                    } catch (IOException ignored) {
                    }
                }
                Thread.sleep(1000);
            }
        } catch (Exception ignored) {
        }
    }

    /** USB mode: "adb reverse" lets the phone reach the PC through 127.0.0.1. */
    private void adbLoop() {
        while (running) {
            if (usbMode) {
                try {
                    Process p = new ProcessBuilder("adb", "reverse", "tcp:" + STREAM_PORT, "tcp:" + STREAM_PORT)
                            .redirectErrorStream(true).redirectOutput(ProcessBuilder.Redirect.DISCARD).start();
                    if (!p.waitFor(8, java.util.concurrent.TimeUnit.SECONDS)) p.destroyForcibly();
                } catch (Exception e) {
                    return; // no adb installed: USB mode is simply not available
                }
            }
            try {
                Thread.sleep(3000);
            } catch (InterruptedException e) {
                return;
            }
        }
    }
}
