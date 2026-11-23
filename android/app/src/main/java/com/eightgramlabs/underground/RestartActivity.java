package com.eightgramlabs.underground;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;

/** Runs in its own process (":restart"): ends the game's process, then opens the game again. */
public class RestartActivity extends Activity {
    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        int pid = getIntent().getIntExtra("pid", 0);
        if (pid != 0) android.os.Process.killProcess(pid);
        try { Thread.sleep(300); } catch (InterruptedException e) { }
        Intent i = new Intent(this, UndergroundActivity.class);
        i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
        startActivity(i);
        finish();
        Runtime.getRuntime().exit(0);
    }
}
