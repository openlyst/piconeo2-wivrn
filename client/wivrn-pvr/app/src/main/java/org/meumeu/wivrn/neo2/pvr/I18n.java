package org.meumeu.wivrn.neo2.pvr;

import android.content.Context;
import android.content.SharedPreferences;
import android.content.res.Configuration;
import android.content.res.Resources;
import android.util.Log;

import java.util.Locale;

public class I18n {
    private static final String TAG = "WiVRn-I18n";
    private static final String PREFS = "wivrn_settings";
    private static final String KEY_LANG = "language";

    public static final int LANG_SYSTEM = 0;
    public static final int LANG_ENGLISH = 1;
    public static final int LANG_CHINESE = 2;

    private static I18n instance;
    private final Context context;
    private int currentLang = LANG_SYSTEM;
    private Locale overrideLocale = null;

    private I18n(Context context) {
        this.context = context.getApplicationContext();
        loadLanguage();
    }

    public static synchronized I18n init(Context context) {
        if (instance == null) {
            instance = new I18n(context);
        }
        return instance;
    }

    public static I18n getInstance() {
        if (instance == null)
            throw new IllegalStateException("I18n not initialized");
        return instance;
    }

    public void loadLanguage() {
        SharedPreferences sp = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        currentLang = sp.getInt(KEY_LANG, LANG_SYSTEM);
        applyLocale();
    }

    public void setLanguage(int lang) {
        currentLang = lang;
        SharedPreferences sp = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        sp.edit().putInt(KEY_LANG, lang).apply();
        applyLocale();
    }

    public int getLanguage() {
        return currentLang;
    }

    private void applyLocale() {
        switch (currentLang) {
            case LANG_ENGLISH:
                overrideLocale = Locale.ENGLISH;
                break;
            case LANG_CHINESE:
                overrideLocale = Locale.SIMPLIFIED_CHINESE;
                break;
            default:
                overrideLocale = null;
                break;
        }
    }

    public String s(int resId) {
        Resources res = getLocalizedResources();
        return res.getString(resId);
    }

    public String s(int resId, Object... formatArgs) {
        Resources res = getLocalizedResources();
        return res.getString(resId, formatArgs);
    }

    private Resources getLocalizedResources() {
        if (overrideLocale == null)
            return context.getResources();

        Configuration config = new Configuration(context.getResources().getConfiguration());
        config.setLocale(overrideLocale);
        return context.createConfigurationContext(config).getResources();
    }

    public static String[] languageNames(Context ctx) {
        I18n i = init(ctx);
        return new String[]{
            i.s(R.string.lang_system),
            "English",
            "简体中文"
        };
    }
}
