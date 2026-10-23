package hu.phonecam.linux;

import java.util.ArrayList;
import java.util.List;

/** What the phone reports about itself ("key=value" lines), same format as on Windows. */
final class PhoneInfo {
    static final class Camera {
        String id = "", label = "";
        final List<int[]> resolutions = new ArrayList<>();
        final List<Integer> fps = new ArrayList<>();
    }

    int battery = -1;
    boolean charging;
    String camera = "";
    int width, height, fps, codec, bitrate;
    boolean stabilization, torch, hasFlash;
    double zoom = 1, zoomMin = 1, zoomMax = 1;
    final List<Camera> cameras = new ArrayList<>();

    static PhoneInfo parse(String text) {
        PhoneInfo info = new PhoneInfo();
        for (String line : text.split("\n")) {
            int eq = line.indexOf('=');
            if (eq <= 0) continue;
            String key = line.substring(0, eq), value = line.substring(eq + 1);
            try {
                switch (key) {
                    case "battery": info.battery = Integer.parseInt(value); break;
                    case "charging": info.charging = value.equals("1"); break;
                    case "camera": info.camera = value; break;
                    case "width": info.width = Integer.parseInt(value); break;
                    case "height": info.height = Integer.parseInt(value); break;
                    case "fps": info.fps = Integer.parseInt(value); break;
                    case "codec": info.codec = Integer.parseInt(value); break;
                    case "bitrate": info.bitrate = Integer.parseInt(value); break;
                    case "stab": info.stabilization = value.equals("1"); break;
                    case "torch": info.torch = value.equals("1"); break;
                    case "flash": info.hasFlash = value.equals("1"); break;
                    case "zoom": info.zoom = Double.parseDouble(value); break;
                    case "zoommin": info.zoomMin = Double.parseDouble(value); break;
                    case "zoommax": info.zoomMax = Double.parseDouble(value); break;
                    case "cam": {
                        String[] parts = value.split("\\|", -1);
                        if (parts.length < 4) break;
                        Camera c = new Camera();
                        c.id = parts[0];
                        c.label = parts[1];
                        for (String r : parts[2].split(",")) {
                            String[] wh = r.split("x");
                            if (wh.length == 2)
                                c.resolutions.add(new int[]{Integer.parseInt(wh[0]), Integer.parseInt(wh[1])});
                        }
                        for (String f : parts[3].split(","))
                            if (!f.isEmpty()) c.fps.add(Integer.parseInt(f));
                        info.cameras.add(c);
                        break;
                    }
                    default:
                        break;
                }
            } catch (NumberFormatException ignored) {
            }
        }
        return info;
    }

    Camera currentCamera() {
        for (Camera c : cameras) if (c.id.equals(camera)) return c;
        return cameras.isEmpty() ? null : cameras.get(0);
    }
}
