package hu.phonecam;

import java.util.Locale;

/** UI language: Hungarian or English, chosen in the settings (default: phone language). */
final class Lang {
    static volatile boolean english = !"hu".equals(Locale.getDefault().getLanguage());

    static String t(String hu, String en) {
        return english ? en : hu;
    }

    private Lang() {
    }
}
