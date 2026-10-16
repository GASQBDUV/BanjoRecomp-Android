package io.github.banjorecomp;

import android.app.Presentation;
import android.os.Bundle;
import android.view.Display;
import android.view.KeyEvent;
import android.view.Window;

public class DualScreenStatsPresentation extends Presentation implements DualScreenStatsManager.StatsSink {
    public interface DebugKeyHandler {
        boolean onDebugKey(int keyCode, KeyEvent event);
    }

    private DualScreenStatsView statsView;
    private final DebugKeyHandler debugKeyHandler;
    private final DualScreenStatsView.DebugAreaButtonHandler debugAreaButtonHandler;

    public DualScreenStatsPresentation(android.content.Context outerContext, Display display,
                                       DebugKeyHandler debugKeyHandler,
                                       DualScreenStatsView.DebugAreaButtonHandler debugAreaButtonHandler) {
        super(outerContext, display);
        this.debugKeyHandler = debugKeyHandler;
        this.debugAreaButtonHandler = debugAreaButtonHandler;
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        if (debugKeyHandler != null && debugKeyHandler.onDebugKey(event.getKeyCode(), event)) {
            return true;
        }
        return super.dispatchKeyEvent(event);
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        Window window = getWindow();
        if (window != null) {
            window.setBackgroundDrawableResource(android.R.color.black);
            // A Presentation is a Dialog, and a Dialog's window is focusable
            // by default. A controller follows the focused display, so the
            // first tap on the panel took the controller away from the game
            // (and, since the game's window lost focus, muted it and hid this
            // panel). Non-focusable windows still receive touch.
            window.addFlags(android.view.WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE);
        }

        statsView = new DualScreenStatsView(getContext(), debugAreaButtonHandler);
        setContentView(statsView);
        showLogo();
    }

    public void showLogo() {
        if (statsView != null) {
            statsView.showLogo();
        }
    }

    public void showBlank() {
        if (statsView != null) {
            statsView.showBlank();
        }
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
}
