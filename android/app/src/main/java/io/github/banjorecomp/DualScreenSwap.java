package io.github.banjorecomp;

import android.app.Activity;
import android.content.Context;
import android.hardware.display.DisplayManager;
import android.view.Display;

import java.io.File;

/**
 * Swapped-screens layout: the game on the secondary display and the stats
 * panel on the default one.
 *
 * On the AYN Thor the default display is the top panel, so the game goes there
 * and the stats Presentation on the bottom one. GammaOS on the Anbernic RG DS /
 * RG DS Plus reports the bottom (touch) panel as the default display, which
 * puts the game on the touch screen and the stats on the screen without touch.
 *
 * Swapping is on by default in this build. Create an empty file named
 * {@code no_swap_screens} in Android/data/com.aure.banjorecomp/files/ to turn
 * it off (Thor-style layout).
 */
final class DualScreenSwap {
    static final String DISABLE_FILE = "no_swap_screens";

    private DualScreenSwap() {}

    static boolean wanted(Context context) {
        File dir = context.getExternalFilesDir(null);
        return dir == null || !new File(dir, DISABLE_FILE).exists();
    }

    /** First valid non-default display, or null on a single-screen device. */
    static Display secondaryDisplay(Context context) {
        DisplayManager dm = (DisplayManager) context.getSystemService(Context.DISPLAY_SERVICE);
        if (dm == null) {
            return null;
        }
        for (Display d : dm.getDisplays()) {
            if (d != null && d.isValid() && d.getDisplayId() != Display.DEFAULT_DISPLAY) {
                return d;
            }
        }
        return null;
    }

    /** Whether this activity actually runs on a secondary display. */
    static boolean isSwapped(Activity activity) {
        Display d = activity.getWindowManager().getDefaultDisplay();
        return d != null && d.getDisplayId() != Display.DEFAULT_DISPLAY;
    }
}
