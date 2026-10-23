package hu.phonecam.linux;

import java.util.Locale;
import java.util.prefs.Preferences;

/** UI language (Hungarian / English), remembered between runs. */
final class Lang {
    private static final Preferences PREFS = Preferences.userNodeForPackage(Lang.class);
    static volatile boolean english = PREFS.getBoolean("english",
            !"hu".equals(Locale.getDefault().getLanguage()));

    static String t(String hu, String en) {
        return english ? en : hu;
    }

    static void set(boolean en) {
        english = en;
        PREFS.putBoolean("english", en);
    }

    private Lang() {
    }
}
