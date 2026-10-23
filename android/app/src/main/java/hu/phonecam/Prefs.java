package hu.phonecam;

import android.content.Context;
import android.content.SharedPreferences;

/** User settings, persisted between launches. */
final class Prefs {
    private final SharedPreferences sp;

    String cameraId;
    int width, height, fps;
    int codec;
    int bitrateMbps; // 0 = automatic
    boolean stabilization;
    String manualIp;
    String language; // "hu", "en" or "" = phone language

    Prefs(Context ctx) {
        sp = ctx.getSharedPreferences("phonecam", Context.MODE_PRIVATE);
        cameraId = sp.getString("camera", "");
        width = sp.getInt("width", 1920);
        height = sp.getInt("height", 1080);
        fps = sp.getInt("fps", 60);
        codec = sp.getInt("codec", StreamConfig.CODEC_H264);
        bitrateMbps = sp.getInt("bitrate", 0);
        stabilization = sp.getBoolean("stabilization", false);
        manualIp = sp.getString("manualIp", "");
        language = sp.getString("language", "");
        applyLanguage();
    }

    void applyLanguage() {
        if (!language.isEmpty()) Lang.english = language.equals("en");
    }

    void save() {
        sp.edit()
                .putString("camera", cameraId)
                .putInt("width", width)
                .putInt("height", height)
                .putInt("fps", fps)
                .putInt("codec", codec)
                .putInt("bitrate", bitrateMbps)
                .putBoolean("stabilization", stabilization)
                .putString("manualIp", manualIp)
                .putString("language", language)
                .apply();
    }
}
