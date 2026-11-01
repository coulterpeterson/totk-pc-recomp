package com.totkrecomp;

import org.libsdl.app.SDLActivity;

// SDL's activity with the port's library and command line. SDL_main (runtime/android.c) is in libtotk.so, the last
// library listed. The launch intent's "args" extra is the command line, split at spaces, so a probe runs with
//   adb shell am start -n com.totkrecomp/.MainActivity --es args "--probe-kernel"
public class MainActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] {"SDL3", "totk"};
    }

    @Override
    protected String[] getArguments() {
        String args = getIntent() != null ? getIntent().getStringExtra("args") : null;
        return args == null || args.trim().isEmpty() ? new String[0] : args.trim().split("\\s+");
    }
}
