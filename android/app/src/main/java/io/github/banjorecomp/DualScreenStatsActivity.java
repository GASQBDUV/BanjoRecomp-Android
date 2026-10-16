package io.github.banjorecomp;

import android.app.Activity;
import android.graphics.Color;
import android.os.Bundle;
import android.util.Log;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowManager;

import org.libsdl.app.SDLControllerManager;

/**
 * Hosts the stats panel on the DEFAULT display for the swapped layout. A
 * Presentation can't be used there: the framework refuses presentation
 * windows on the default display.
 *
 * The window has to be focusable: an activity that is the only window of its
 * task, with no focusable window, makes the input dispatcher ANR ("Application
 * does not have a focused window"). Being focusable, and started after the
 * game, it takes the focused display, and a controller follows the focused
 * display, so controller input is forwarded to the game below.
 */
public class DualScreenStatsActivity extends Activity implements DualScreenStatsManager.StatsSink {
    private static final String TAG = "DualScreenStats";

    static volatile DualScreenStatsActivity instance;

    private DualScreenStatsView statsView;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        try {
            getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
            getWindow().getDecorView().setBackgroundColor(Color.BLACK);
            applyImmersive();
            statsView = new DualScreenStatsView(this, null);
            setContentView(statsView);
            statsView.showLogo();
        } catch (Throwable e) {
            // Shares the game's process; drop the panel rather than the game.
            Log.e(TAG, "Stats activity failed to come up", e);
            finish();
            return;
        }
        instance = this;
        DualScreenStatsManager manager = DualScreenStatsManager.current();
        if (manager != null) {
            manager.onStatsActivityReady(this);
        }
        Log.i(TAG, "Stats activity up on display "
                + getWindowManager().getDefaultDisplay().getDisplayId());
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) {
            applyImmersive();
        }
    }

    private void applyImmersive() {
        getWindow().getDecorView().setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                        | View.SYSTEM_UI_FLAG_FULLSCREEN
                        | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                        | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                        | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION);
    }

    @Override
    public void updateStats(DualScreenStats stats) {
        if (statsView != null) {
            statsView.updateStats(stats);
        }
    }

    @Override
    public void setTheme(BanjoSpriteTheme theme) {
        if (statsView != null) {
            statsView.setTheme(theme);
        }
    }

    // Controller input that lands here belongs to the game; hand it to the
    // game activity's own dispatch, which still has its internal view focus.
    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        if (isFromController(event.getDevice())) {
            Activity game = BanjoSDLActivity.current();
            if (game != null && game.dispatchKeyEvent(event)) {
                return true;
            }
        }
        return super.dispatchKeyEvent(event);
    }

    // Sticks and triggers, same reasoning.
    @Override
    public boolean dispatchGenericMotionEvent(MotionEvent event) {
        if (SDLControllerManager.isDeviceSDLJoystick(event.getDeviceId())
                && SDLControllerManager.handleJoystickMotionEvent(event)) {
            return true;
        }
        return super.dispatchGenericMotionEvent(event);
    }

    private static boolean isFromController(InputDevice device) {
        if (device == null) {
            return false;
        }
        int sources = device.getSources();
        return (sources & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
                || (sources & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK
                || (sources & InputDevice.SOURCE_DPAD) == InputDevice.SOURCE_DPAD;
    }

    @Override
    public void onBackPressed() {
        // Swallow BACK: a gamepad B can fall back to KEYCODE_BACK, and closing
        // the panel mid-game would leave no way to get it back. The game takes
        // the panel down itself when it leaves the foreground.
    }

    @Override
    protected void onDestroy() {
        if (instance == this) {
            instance = null;
        }
        super.onDestroy();
    }
}
