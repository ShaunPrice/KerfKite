package au.cncstudio.pocket;

import android.app.Activity;
import android.content.Context;
import android.content.SharedPreferences;
import android.view.Window;
import android.view.WindowManager;
import android.widget.Switch;

/** A shared device preference applied only to the currently resumed Pocket window. */
final class ScreenAwakeSwitch extends Switch implements SharedPreferences.OnSharedPreferenceChangeListener {
    private static final String KEY = "keep-screen-awake";
    private final SharedPreferences preferences;
    private final Window window;
    private boolean resumed;

    ScreenAwakeSwitch(Activity activity) {
        super(activity);
        preferences = activity.getSharedPreferences("display", Context.MODE_PRIVATE);
        window = activity.getWindow();
        setText(R.string.keep_screen_awake);
        setTextColor(0xff203a39);
        setMinHeight(Math.round(48 * getResources().getDisplayMetrics().density));
        setSaveEnabled(false);
        setOnCheckedChangeListener((button, checked) -> {
            if (resumed && preferences.getBoolean(KEY, false) != checked) {
                preferences.edit().putBoolean(KEY, checked).apply();
            }
        });
    }

    void resume() {
        resumed = true;
        preferences.registerOnSharedPreferenceChangeListener(this);
        refresh();
    }

    void pause() {
        resumed = false;
        preferences.unregisterOnSharedPreferenceChangeListener(this);
        window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
    }

    @Override public void onSharedPreferenceChanged(SharedPreferences prefs, String key) {
        if (resumed && KEY.equals(key)) refresh();
    }

    private void refresh() {
        boolean enabled = preferences.getBoolean(KEY, false);
        setChecked(enabled);
        if (enabled) window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        else window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
    }
}
