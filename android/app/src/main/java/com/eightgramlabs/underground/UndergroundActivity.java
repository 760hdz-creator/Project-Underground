package com.eightgramlabs.underground;

import android.content.Intent;
import org.libsdl.app.SDLActivity;

/** Project Underground: SDL's activity (libmain.so holds the game, built on the player's PC from their own disc). */
public class UndergroundActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] {"SDL3", "main"};
    }

    /** Called from native code (HalReturnToFirmware): the game restarts itself, as the Xbox reboots into it. */
    public void relaunch() {
        Intent i = new Intent(this, RestartActivity.class);
        i.putExtra("pid", android.os.Process.myPid());
        i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        startActivity(i);
    }
}
