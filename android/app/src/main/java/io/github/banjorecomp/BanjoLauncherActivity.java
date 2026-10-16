package io.github.banjorecomp;

import android.app.Activity;
import android.app.ActivityOptions;
import android.content.Intent;
import android.os.Build;
import android.os.Bundle;
import android.util.Log;
import android.view.Display;

/**
 * Launcher entry point. Starts the game, on the secondary display when the
 * swapped layout is wanted (see {@link DualScreenSwap}), then gets out of the
 * way. A trampoline is needed because the game activity is singleTask: it
 * can't move itself to another display once it exists.
 */
public class BanjoLauncherActivity extends Activity {
    private static final String TAG = "BanjoLauncher";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        Intent intent = new Intent(this, BanjoSDLActivity.class);
        if (getIntent() != null && getIntent().getExtras() != null) {
            intent.putExtras(getIntent().getExtras());
        }
        Display target = Build.VERSION.SDK_INT >= 26 && DualScreenSwap.wanted(this)
                ? DualScreenSwap.secondaryDisplay(this) : null;
        if (target != null) {
            intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            ActivityOptions options = ActivityOptions.makeBasic();
            options.setLaunchDisplayId(target.getDisplayId());
            try {
                startActivity(intent, options.toBundle());
                Log.i(TAG, "Launching game on display " + target.getDisplayId() + " (swapped)");
                finish();
                return;
            } catch (RuntimeException e) {
                // Launching on another display can be refused; fall back to a
                // normal launch rather than not starting at all.
                Log.w(TAG, "Launch on display " + target.getDisplayId() + " refused", e);
            }
        }
        startActivity(intent);
        finish();
    }
}
