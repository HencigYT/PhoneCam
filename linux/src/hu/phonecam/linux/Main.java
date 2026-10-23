package hu.phonecam.linux;

import javax.swing.BorderFactory;
import javax.swing.Box;
import javax.swing.BoxLayout;
import javax.swing.JButton;
import javax.swing.JCheckBox;
import javax.swing.JComboBox;
import javax.swing.JComponent;
import javax.swing.JFrame;
import javax.swing.JLabel;
import javax.swing.JOptionPane;
import javax.swing.JPanel;
import javax.swing.JSlider;
import javax.swing.SwingUtilities;
import javax.swing.Timer;
import javax.swing.UIManager;
import java.awt.BorderLayout;
import java.awt.Color;
import java.awt.Dimension;
import java.awt.FlowLayout;
import java.awt.Font;
import java.awt.Graphics;
import java.awt.Graphics2D;
import java.awt.GridLayout;
import java.awt.RenderingHints;
import java.awt.image.BufferedImage;
import java.util.Locale;
import java.util.prefs.Preferences;

/** PhoneCam receiver for Linux: phone stream -> ffmpeg -> v4l2loopback virtual camera. */
public final class Main {
    static final Color BG = new Color(0x11, 0x13, 0x18);
    static final Color CARD = new Color(0x1A, 0x1D, 0x24);
    static final Color TEXT = new Color(0xE8, 0xEA, 0xED);
    static final Color MUTED = new Color(0x9A, 0xA0, 0xA6);
    static final Color ACCENT = new Color(0x3B, 0x82, 0xF6);
    static final Color GREEN = new Color(0x22, 0xC5, 0x5E);
    static final Color YELLOW = new Color(0xEA, 0xB3, 0x08);
    static final Color RED = new Color(0xEF, 0x44, 0x44);
    static final int[] BITRATES = {0, 6, 10, 15, 20, 30, 40, 60, 80};
    static final int[][] OUTPUTS = {{1920, 1080}, {1280, 720}, {3840, 2160}, {2560, 1440}};

    static final String SETUP_SCRIPT =
            "modprobe -r v4l2loopback 2>/dev/null; "
            + "modprobe v4l2loopback devices=1 video_nr=42 card_label=PhoneCam exclusive_caps=1 && "
            + "echo 'options v4l2loopback devices=1 video_nr=42 card_label=PhoneCam exclusive_caps=1' "
            + "> /etc/modprobe.d/phonecam.conf && echo v4l2loopback > /etc/modules-load.d/phonecam.conf";

    private final Preferences prefs = Preferences.userNodeForPackage(Main.class);
    private Server server;
    private Sink sink;
    private JFrame frame;
    private final Preview preview = new Preview();
    private JLabel title, subtitle, status, phoneName, phoneLink, phoneStream, battery;
    private JLabel statFps, statRate, statRtt, deviceLine, addressLine, errorLine;
    private JLabel lCamera, lRes, lFps, lCodec, lBitrate, lZoom, lOutput, zoomValue;
    private JLabel cardPhone, cardSettings, cardControls, cardPc, cardInfo;
    private JComboBox<String> cbCamera = new JComboBox<>(), cbRes = new JComboBox<>(), cbFps = new JComboBox<>(),
            cbCodec = new JComboBox<>(), cbBitrate = new JComboBox<>(), cbOutput = new JComboBox<>();
    private JCheckBox chStab, chTorch, chMirror, chFlip, chPreview, chUsb;
    private JSlider zoom = new JSlider(0, 1000, 0);
    private JButton btnHu, btnEn, btnSetup;
    private PhoneInfo phone;
    private long phoneSerial = -1;
    private boolean populating;
    private long lastZoomSend;

    public static void main(String[] args) throws Exception {
        String output = null, ffmpeg = System.getenv().getOrDefault("PHONECAM_FFMPEG", "ffmpeg");
        boolean beacons = true;
        for (int i = 0; i < args.length; i++) {
            switch (args[i]) {
                case "--setup":
                    System.exit(runSetup(false));
                    return;
                case "--output":
                    output = args[++i];
                    break;
                case "--ffmpeg":
                    ffmpeg = args[++i];
                    break;
                case "--no-beacon":
                    beacons = false;
                    break;
                default:
                    break;
            }
        }
        String out = output != null ? output : Sink.findDevice();
        String ff = ffmpeg;
        boolean bc = beacons;
        SwingUtilities.invokeLater(() -> new Main().start(out, ff, bc));
    }

    /** Loads v4l2loopback as a camera called "PhoneCam" and makes it permanent (needs root). */
    static int runSetup(boolean graphical) {
        try {
            ProcessBuilder pb = graphical ? new ProcessBuilder("pkexec", "sh", "-c", SETUP_SCRIPT)
                    : new ProcessBuilder("sudo", "sh", "-c", SETUP_SCRIPT).inheritIO();
            Process p = pb.start();
            int code = p.waitFor();
            if (!graphical) {
                System.out.println(code == 0 ? "PhoneCam virtual camera ready: /dev/video42"
                        : "Failed. Install v4l2loopback first, e.g.:\n  sudo apt install v4l2loopback-dkms ffmpeg\n"
                        + "  sudo dnf install v4l2loopback ffmpeg\n  sudo pacman -S v4l2loopback-dkms ffmpeg");
            }
            return code;
        } catch (Exception e) {
            if (!graphical) System.out.println("Setup failed: " + e.getMessage());
            return 1;
        }
    }

    // ------------------------------------------------------------------ UI

    private static void darkTheme() {
        try {
            UIManager.put("control", CARD);
            UIManager.put("info", CARD);
            UIManager.put("nimbusBase", new Color(0x23, 0x27, 0x30));
            UIManager.put("nimbusBlueGrey", new Color(0x2A, 0x2F, 0x39));
            UIManager.put("nimbusLightBackground", new Color(0x21, 0x25, 0x2E));
            UIManager.put("nimbusFocus", ACCENT);
            UIManager.put("nimbusSelectionBackground", ACCENT);
            UIManager.put("nimbusSelectedText", Color.WHITE);
            UIManager.put("text", TEXT);
            UIManager.put("nimbusDisabledText", new Color(0x5F, 0x66, 0x70));
            UIManager.put("ComboBox.foreground", TEXT);
            UIManager.setLookAndFeel("javax.swing.plaf.nimbus.NimbusLookAndFeel");
        } catch (Exception ignored) {
        }
    }

    private static JLabel label(String text, float size, Color color, boolean bold) {
        JLabel l = new JLabel(text);
        l.setForeground(color);
        l.setFont(l.getFont().deriveFont(bold ? Font.BOLD : Font.PLAIN, size));
        return l;
    }

    private static JPanel card() {
        JPanel p = new JPanel();
        p.setBackground(CARD);
        p.setLayout(new BoxLayout(p, BoxLayout.Y_AXIS));
        p.setBorder(BorderFactory.createCompoundBorder(BorderFactory.createLineBorder(new Color(0x2A, 0x2F, 0x39)),
                BorderFactory.createEmptyBorder(10, 14, 12, 14)));
        return p;
    }

    private static JPanel row(JComponent left, JComponent right) {
        JPanel r = new JPanel(new BorderLayout(10, 0));
        r.setOpaque(false);
        r.setAlignmentX(0);
        r.setMaximumSize(new Dimension(Integer.MAX_VALUE, 34));
        r.add(left, BorderLayout.WEST);
        r.add(right, BorderLayout.CENTER);
        return r;
    }

    private JCheckBox check(String key, boolean def) {
        JCheckBox c = new JCheckBox();
        c.setOpaque(false);
        c.setForeground(TEXT);
        c.setAlignmentX(0);
        c.setSelected(key == null ? def : prefs.getBoolean(key, def));
        return c;
    }

    private void start(String output, String ffmpeg, boolean beacons) {
        darkTheme();
        sink = new Sink(ffmpeg, output, img -> SwingUtilities.invokeLater(() -> preview.set(img)));
        int outIndex = prefs.getInt("output", 0);
        sink.outWidth = OUTPUTS[outIndex][0];
        sink.outHeight = OUTPUTS[outIndex][1];
        server = new Server(sink);
        server.beacons = beacons;

        frame = new JFrame("PhoneCam");
        frame.setDefaultCloseOperation(JFrame.EXIT_ON_CLOSE);
        JPanel root = new JPanel(new BorderLayout(16, 12));
        root.setBackground(BG);
        root.setBorder(BorderFactory.createEmptyBorder(14, 20, 18, 20));
        frame.setContentPane(root);

        // header
        JPanel header = new JPanel(new BorderLayout());
        header.setOpaque(false);
        JPanel titles = new JPanel(new FlowLayout(FlowLayout.LEFT, 12, 0));
        titles.setOpaque(false);
        title = label("PhoneCam", 24, TEXT, true);
        subtitle = label("", 13, MUTED, false);
        titles.add(title);
        titles.add(subtitle);
        header.add(titles, BorderLayout.WEST);
        JPanel right = new JPanel(new FlowLayout(FlowLayout.RIGHT, 8, 0));
        right.setOpaque(false);
        btnHu = new JButton("HU");
        btnEn = new JButton("EN");
        btnHu.addActionListener(e -> setLanguage(false));
        btnEn.addActionListener(e -> setLanguage(true));
        status = label("", 13, TEXT, false);
        right.add(status);
        right.add(btnHu);
        right.add(btnEn);
        header.add(right, BorderLayout.EAST);
        root.add(header, BorderLayout.NORTH);

        // left: preview, stats, camera info
        JPanel left = new JPanel();
        left.setOpaque(false);
        left.setLayout(new BoxLayout(left, BoxLayout.Y_AXIS));
        preview.setPreferredSize(new Dimension(640, 360));
        preview.setMaximumSize(new Dimension(640, 360));
        preview.setAlignmentX(0);
        left.add(preview);
        left.add(Box.createVerticalStrut(10));
        JPanel stats = new JPanel(new GridLayout(1, 3, 10, 0));
        stats.setOpaque(false);
        stats.setAlignmentX(0);
        stats.setMaximumSize(new Dimension(640, 70));
        statFps = label("–", 17, TEXT, true);
        statRate = label("–", 17, TEXT, true);
        statRtt = label("–", 17, TEXT, true);
        stats.add(tile("fps", statFps));
        stats.add(tile("rate", statRate));
        stats.add(tile("rtt", statRtt));
        left.add(stats);
        left.add(Box.createVerticalStrut(10));
        JPanel info = card();
        info.setAlignmentX(0);
        info.setMaximumSize(new Dimension(640, 140));
        cardInfo = label("", 15, TEXT, true);
        deviceLine = label("", 13, MUTED, false);
        addressLine = label("", 12, MUTED, false);
        errorLine = label("", 12, RED, false);
        btnSetup = new JButton();
        btnSetup.addActionListener(e -> setupFromUi());
        info.add(cardInfo);
        info.add(Box.createVerticalStrut(6));
        info.add(deviceLine);
        info.add(addressLine);
        info.add(errorLine);
        info.add(Box.createVerticalStrut(6));
        info.add(btnSetup);
        left.add(info);
        root.add(left, BorderLayout.CENTER);

        // right: phone, settings, controls, PC options
        JPanel side = new JPanel();
        side.setOpaque(false);
        side.setLayout(new BoxLayout(side, BoxLayout.Y_AXIS));
        side.setPreferredSize(new Dimension(340, 600));

        JPanel phoneCard = card();
        cardPhone = label("", 15, TEXT, true);
        phoneName = label("", 16, TEXT, true);
        phoneLink = label("", 13, MUTED, false);
        phoneStream = label("", 13, MUTED, false);
        battery = label("", 18, TEXT, true);
        phoneCard.add(phoneName);
        phoneCard.add(phoneLink);
        phoneCard.add(phoneStream);
        phoneCard.add(battery);
        side.add(phoneCard);
        side.add(Box.createVerticalStrut(10));

        JPanel settings = card();
        cardSettings = label("", 15, TEXT, true);
        settings.add(cardSettings);
        lCamera = label("", 13, MUTED, false);
        lRes = label("", 13, MUTED, false);
        lFps = label("", 13, MUTED, false);
        lCodec = label("", 13, MUTED, false);
        lBitrate = label("", 13, MUTED, false);
        for (JLabel l : new JLabel[]{lCamera, lRes, lFps, lCodec, lBitrate}) l.setPreferredSize(new Dimension(95, 26));
        settings.add(row(lCamera, cbCamera));
        settings.add(row(lRes, cbRes));
        settings.add(row(lFps, cbFps));
        settings.add(row(lCodec, cbCodec));
        settings.add(row(lBitrate, cbBitrate));
        chStab = check(null, false);
        settings.add(chStab);
        side.add(settings);
        side.add(Box.createVerticalStrut(10));

        JPanel controls = card();
        cardControls = label("", 15, TEXT, true);
        controls.add(cardControls);
        chTorch = check(null, false);
        controls.add(chTorch);
        lZoom = label("Zoom", 13, MUTED, false);
        zoomValue = label("–", 13, TEXT, true);
        zoom.setOpaque(false);
        JPanel zoomRow = row(lZoom, zoom);
        zoomRow.add(zoomValue, BorderLayout.EAST);
        controls.add(zoomRow);
        side.add(controls);
        side.add(Box.createVerticalStrut(10));

        JPanel pc = card();
        cardPc = label("", 15, TEXT, true);
        pc.add(cardPc);
        chMirror = check("mirror", false);
        chFlip = check("flip", false);
        chPreview = check("preview", true);
        chUsb = check("usb", true);
        JPanel grid = new JPanel(new GridLayout(2, 2));
        grid.setOpaque(false);
        grid.setAlignmentX(0);
        grid.add(chMirror);
        grid.add(chFlip);
        grid.add(chPreview);
        grid.add(chUsb);
        pc.add(grid);
        lOutput = label("", 13, MUTED, false);
        lOutput.setPreferredSize(new Dimension(95, 26));
        for (int[] o : OUTPUTS) cbOutput.addItem(o[0] + "×" + o[1]);
        cbOutput.setSelectedIndex(outIndex);
        pc.add(row(lOutput, cbOutput));
        side.add(pc);
        root.add(side, BorderLayout.EAST);

        // behaviour
        server.mirror = chMirror.isSelected();
        server.flip = chFlip.isSelected();
        server.preview = chPreview.isSelected();
        server.usbMode = chUsb.isSelected();
        chMirror.addActionListener(e -> { server.mirror = chMirror.isSelected(); prefs.putBoolean("mirror", server.mirror); server.restartVideo(); });
        chFlip.addActionListener(e -> { server.flip = chFlip.isSelected(); prefs.putBoolean("flip", server.flip); server.restartVideo(); });
        chPreview.addActionListener(e -> { server.preview = chPreview.isSelected(); prefs.putBoolean("preview", server.preview); preview.set(null); server.restartVideo(); });
        chUsb.addActionListener(e -> { server.usbMode = chUsb.isSelected(); prefs.putBoolean("usb", server.usbMode); });
        cbOutput.addActionListener(e -> {
            int i = cbOutput.getSelectedIndex();
            prefs.putInt("output", i);
            sink.outWidth = OUTPUTS[i][0];
            sink.outHeight = OUTPUTS[i][1];
            server.restartVideo();
        });
        cbCamera.addActionListener(e -> {
            if (populating || phone == null) return;
            int i = cbCamera.getSelectedIndex();
            if (i >= 0 && i < phone.cameras.size()) server.sendSettings("camera=" + phone.cameras.get(i).id + "\n");
        });
        cbRes.addActionListener(e -> {
            PhoneInfo.Camera c = phone != null ? phone.currentCamera() : null;
            int i = cbRes.getSelectedIndex();
            if (populating || c == null || i < 0 || i >= c.resolutions.size()) return;
            int[] r = c.resolutions.get(i);
            server.sendSettings("width=" + r[0] + "\nheight=" + r[1] + "\n");
        });
        cbFps.addActionListener(e -> {
            PhoneInfo.Camera c = phone != null ? phone.currentCamera() : null;
            int i = cbFps.getSelectedIndex();
            if (populating || c == null || i < 0 || i >= c.fps.size()) return;
            server.sendSettings("fps=" + c.fps.get(i) + "\n");
        });
        cbCodec.addActionListener(e -> {
            if (!populating && phone != null && cbCodec.getSelectedIndex() >= 0)
                server.sendSettings("codec=" + cbCodec.getSelectedIndex() + "\n");
        });
        cbBitrate.addActionListener(e -> {
            int i = cbBitrate.getSelectedIndex();
            if (!populating && phone != null && i >= 0) server.sendSettings("bitrate=" + BITRATES[i] + "\n");
        });
        chStab.addActionListener(e -> {
            if (!populating && phone != null) server.sendSettings("stab=" + (chStab.isSelected() ? 1 : 0) + "\n");
        });
        chTorch.addActionListener(e -> {
            if (!populating && phone != null) server.sendSettings("torch=" + (chTorch.isSelected() ? 1 : 0) + "\n");
        });
        zoom.addChangeListener(e -> {
            if (populating || phone == null) return;
            double z = posToZoom(zoom.getValue());
            zoomValue.setText(String.format(Locale.US, "%.1f×", z));
            long now = System.currentTimeMillis();
            if (!zoom.getValueIsAdjusting() || now - lastZoomSend > 60) {
                lastZoomSend = now;
                server.sendSettings(String.format(Locale.US, "zoom=%.2f\n", z));
            }
        });

        applyTexts();
        populate();
        frame.pack();
        frame.setLocationRelativeTo(null);
        frame.setVisible(true);

        if (!sink.available()) {
            errorLine.setText(Lang.t("Nincs ffmpeg – telepítsd: sudo apt install ffmpeg",
                    "ffmpeg missing – install it: sudo apt install ffmpeg"));
        }
        try {
            server.start();
        } catch (Exception e) {
            JOptionPane.showMessageDialog(frame, Lang.t("A " + Server.STREAM_PORT + " TCP port foglalt (fut már egy PhoneCam?)",
                    "TCP port " + Server.STREAM_PORT + " is in use (is PhoneCam already running?)"), "PhoneCam",
                    JOptionPane.ERROR_MESSAGE);
        }
        new Timer(250, e -> refresh()).start();
    }

    private JPanel tile(String which, JLabel value) {
        JPanel t = card();
        JLabel name = label("", 12, MUTED, false);
        name.putClientProperty("tile", which);
        t.add(name);
        t.add(value);
        tileNames.add(name);
        return t;
    }

    private final java.util.List<JLabel> tileNames = new java.util.ArrayList<>();

    private void setLanguage(boolean english) {
        Lang.set(english);
        applyTexts();
        populate();
        refresh();
    }

    private void applyTexts() {
        subtitle.setText(Lang.t("a telefonod mint webkamera", "your phone as a webcam"));
        for (JLabel l : tileNames) {
            String w = (String) l.getClientProperty("tile");
            l.setText(w.equals("fps") ? Lang.t("Képkocka/mp", "Frame rate")
                    : w.equals("rate") ? Lang.t("Bitráta", "Bitrate") : Lang.t("Hálózat (RTT)", "Network (RTT)"));
        }
        cardInfo.setText(Lang.t("Virtuális kamera", "Virtual camera"));
        btnSetup.setText(Lang.t("Virtuális kamera beállítása (rendszergazda)", "Set up the virtual camera (admin)"));
        cardSettings.setText(Lang.t("Kamera beállítások", "Camera settings"));
        lCamera.setText(Lang.t("Kamera", "Camera"));
        lRes.setText(Lang.t("Felbontás", "Resolution"));
        lFps.setText(Lang.t("Képkocka/mp", "Frame rate"));
        lCodec.setText(Lang.t("Kodek", "Codec"));
        lBitrate.setText(Lang.t("Bitráta", "Bitrate"));
        chStab.setText(Lang.t("Képstabilizálás (több késés)", "Stabilization (adds delay)"));
        cardControls.setText(Lang.t("Vezérlés", "Controls"));
        chTorch.setText(Lang.t("Vaku (zseblámpa)", "Flash (torch)"));
        cardPc.setText(Lang.t("Kép a gépen", "Image on this PC"));
        chMirror.setText(Lang.t("Tükrözés", "Mirror"));
        chFlip.setText(Lang.t("Forgatás 180°", "Rotate 180°"));
        chPreview.setText(Lang.t("Előnézet", "Preview"));
        chUsb.setText("USB (adb)");
        lOutput.setText(Lang.t("Kimenet", "Output"));
        btnHu.setForeground(Lang.english ? MUTED : ACCENT);
        btnEn.setForeground(Lang.english ? ACCENT : MUTED);
    }

    private double posToZoom(int pos) {
        if (phone == null || phone.zoomMax <= phone.zoomMin) return 1;
        double z = Math.exp(Math.log(phone.zoomMin) + (Math.log(phone.zoomMax) - Math.log(phone.zoomMin)) * pos / 1000.0);
        return Math.abs(z - 1) < 0.04 ? 1 : z;
    }

    private int zoomToPos(double z) {
        if (phone == null || phone.zoomMax <= phone.zoomMin) return 0;
        return (int) Math.round((Math.log(z) - Math.log(phone.zoomMin)) / (Math.log(phone.zoomMax) - Math.log(phone.zoomMin)) * 1000);
    }

    /** Fills the phone controls from the latest phone report. */
    private void populate() {
        populating = true;
        boolean on = phone != null;
        for (JComboBox<String> cb : java.util.List.of(cbCamera, cbRes, cbFps, cbCodec, cbBitrate)) {
            cb.removeAllItems();
            cb.setEnabled(on);
        }
        chStab.setEnabled(on);
        chTorch.setEnabled(on && phone.hasFlash);
        zoom.setEnabled(on && phone.zoomMax > phone.zoomMin + 0.01);
        if (on) {
            int sel = 0;
            for (int i = 0; i < phone.cameras.size(); i++) {
                cbCamera.addItem(phone.cameras.get(i).label);
                if (phone.cameras.get(i).id.equals(phone.camera)) sel = i;
            }
            if (!phone.cameras.isEmpty()) cbCamera.setSelectedIndex(sel);
            PhoneInfo.Camera c = phone.currentCamera();
            if (c != null) {
                sel = 0;
                for (int i = 0; i < c.resolutions.size(); i++) {
                    int[] r = c.resolutions.get(i);
                    cbRes.addItem((r[1] == 2160 ? "4K" : r[1] + "p") + "  (" + r[0] + "×" + r[1] + ")");
                    if (r[0] == phone.width && r[1] == phone.height) sel = i;
                }
                if (!c.resolutions.isEmpty()) cbRes.setSelectedIndex(sel);
                sel = 0;
                for (int i = 0; i < c.fps.size(); i++) {
                    cbFps.addItem(c.fps.get(i) + " fps");
                    if (c.fps.get(i) == phone.fps) sel = i;
                }
                if (!c.fps.isEmpty()) cbFps.setSelectedIndex(sel);
            }
            cbCodec.addItem(Lang.t("H.264 – mindenhol megy", "H.264 – works everywhere"));
            cbCodec.addItem(Lang.t("H.265 (HEVC) – jobb kép", "H.265 (HEVC) – better image"));
            cbCodec.setSelectedIndex(phone.codec == 1 ? 1 : 0);
            sel = 0;
            for (int i = 0; i < BITRATES.length; i++) {
                cbBitrate.addItem(BITRATES[i] == 0 ? Lang.t("Automatikus", "Automatic") : BITRATES[i] + " Mbit/s");
                if (BITRATES[i] == phone.bitrate) sel = i;
            }
            cbBitrate.setSelectedIndex(sel);
            chStab.setSelected(phone.stabilization);
            chTorch.setSelected(phone.torch);
            if (!zoom.getValueIsAdjusting()) {
                zoom.setValue(zoomToPos(phone.zoom));
                zoomValue.setText(String.format(Locale.US, "%.1f×", phone.zoom));
            }
        } else {
            zoomValue.setText("–");
        }
        populating = false;
    }

    private void refresh() {
        if (server.infoSerial != phoneSerial) {
            phoneSerial = server.infoSerial;
            phone = server.info;
            populate();
        }
        boolean conn = server.connected;
        status.setText((conn ? "● " + Lang.t("Csatlakozva · ", "Connected · ") + server.phoneName
                : "○ " + Lang.t("Várakozás a telefonra", "Waiting for the phone")));
        status.setForeground(conn ? GREEN : YELLOW);
        if (conn) {
            phoneName.setText(server.phoneName);
            phoneLink.setText(server.usb ? Lang.t("USB kábel", "USB cable") : "Wi-Fi · " + server.phoneAddress);
            phoneStream.setText(server.width > 0 ? server.width + "×" + server.height + " · " + server.fps + " fps · "
                    + (server.codec == 1 ? "H.265" : "H.264") : "");
            PhoneInfo p = phone;
            battery.setText(p == null || p.battery < 0 ? "" : "🔋 " + p.battery + "%"
                    + (p.charging ? Lang.t("  ⚡ töltés", "  ⚡ charging") : ""));
            battery.setForeground(p != null && p.battery >= 0 && p.battery <= 15 ? RED : TEXT);
        } else {
            phoneName.setText(Lang.t("Nincs telefon", "No phone"));
            phoneLink.setText(Lang.t("Indítsd el a PhoneCam appot a telefonon,", "Start the PhoneCam app on your phone,"));
            phoneStream.setText(Lang.t("ugyanazon a Wi-Fi-n vagy USB kábellel.", "on the same Wi-Fi or with a USB cable."));
            battery.setText("");
            preview.set(null);
        }
        boolean live = conn && server.width > 0;
        statFps.setText(live ? String.format(Locale.US, "%.0f / %d", server.inFps, server.fps) : "–");
        statRate.setText(live ? String.format(Locale.US, "%.1f Mbit/s", server.inMbps) : "–");
        statRtt.setText(conn && server.rttMs >= 0 ? String.format(Locale.US, "%.0f ms", server.rttMs) : "–");

        String dev = sink.output();
        if (dev == null) {
            String found = Sink.findDevice();
            if (found != null) {
                sink.setOutput(found);
                server.restartVideo();
                dev = found;
            }
        }
        deviceLine.setText(dev != null
                ? Lang.t("Eszköz: ", "Device: ") + dev + Lang.t("  – válaszd a „PhoneCam” kamerát az appokban",
                "  – pick the “PhoneCam” camera in your apps")
                : Lang.t("Nincs virtuális kamera – kattints a beállításra (v4l2loopback kell)",
                "No virtual camera – click setup (needs v4l2loopback)"));
        deviceLine.setForeground(dev != null ? MUTED : YELLOW);
        btnSetup.setVisible(dev == null);
        addressLine.setText(Lang.t("PC címe: ", "PC address: ") + String.join(",  ", server.addresses));
        String err = sink.lastError;
        if (!err.isEmpty()) errorLine.setText("ffmpeg: " + err);
    }

    private void setupFromUi() {
        btnSetup.setEnabled(false);
        new Thread(() -> {
            int code = runSetup(true);
            SwingUtilities.invokeLater(() -> {
                btnSetup.setEnabled(true);
                if (code != 0)
                    JOptionPane.showMessageDialog(frame, Lang.t(
                            "Nem sikerült. Előbb telepítsd a v4l2loopback-et és az ffmpeg-et, pl.:\n"
                            + "sudo apt install v4l2loopback-dkms ffmpeg",
                            "Failed. Install v4l2loopback and ffmpeg first, e.g.:\n"
                            + "sudo apt install v4l2loopback-dkms ffmpeg"), "PhoneCam", JOptionPane.WARNING_MESSAGE);
                refresh();
            });
        }, "setup").start();
    }

    /** Letterboxed preview of what goes to the virtual camera. */
    private static final class Preview extends JComponent {
        private BufferedImage image;

        void set(BufferedImage img) {
            image = img;
            repaint();
        }

        @Override
        protected void paintComponent(Graphics g0) {
            Graphics2D g = (Graphics2D) g0;
            g.setRenderingHint(RenderingHints.KEY_INTERPOLATION, RenderingHints.VALUE_INTERPOLATION_BILINEAR);
            g.setRenderingHint(RenderingHints.KEY_TEXT_ANTIALIASING, RenderingHints.VALUE_TEXT_ANTIALIAS_ON);
            g.setColor(new Color(0x0C, 0x0E, 0x12));
            g.fillRect(0, 0, getWidth(), getHeight());
            if (image == null) {
                g.setColor(MUTED);
                g.setFont(getFont().deriveFont(Font.BOLD, 18f));
                String s = Lang.t("Nincs kép", "No image");
                g.drawString(s, (getWidth() - g.getFontMetrics().stringWidth(s)) / 2, getHeight() / 2);
                return;
            }
            double scale = Math.min((double) getWidth() / image.getWidth(), (double) getHeight() / image.getHeight());
            int w = (int) (image.getWidth() * scale), h = (int) (image.getHeight() * scale);
            g.drawImage(image, (getWidth() - w) / 2, (getHeight() - h) / 2, w, h, null);
        }
    }
}
