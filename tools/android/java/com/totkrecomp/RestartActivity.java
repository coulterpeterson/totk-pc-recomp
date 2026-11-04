package com.totkrecomp;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.Process;

import java.io.File;

// Restart in the menu (runtime/platform/linux.c plat_restart). A process that is ending cannot bring the app back, and
// Android does not let an alarm start an activity from the background; this activity runs in a process of its own, in
// the foreground, waits for the game's process to end so the new one finds its saves and files free, and starts the game.
public class RestartActivity extends Activity {
    private static final long TIMEOUT_MS = 10000, POLL_MS = 50;

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        final int pid = getIntent().getIntExtra("pid", 0);
        final long end = System.currentTimeMillis() + TIMEOUT_MS;
        final Handler h = new Handler(Looper.getMainLooper());
        h.post(new Runnable() {
            @Override
            public void run() {
                if (pid != 0 && new File("/proc/" + pid).exists() && System.currentTimeMillis() < end) {
                    h.postDelayed(this, POLL_MS);
                    return;
                }
                startActivity(new Intent(RestartActivity.this, MainActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
                finish();
                Process.killProcess(Process.myPid());
            }
        });
    }
}
