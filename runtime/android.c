// Android entry point. SDLActivity (tools/android/) loads libtotk.so and calls SDL_main on a thread of its own; this
// file sets up what main() expects from a desktop process and then runs it (main.c names it totk_main on Android):
// - TOTK_HOME, the app's external files folder (/storage/emulated/0/Android/data/<package>/files). The runtime keeps
//   its files beside its executable (totk.ini, romfs-index.bin, shader caches, logs; plat_exe_path), and an app has
//   none, so platform/linux.c reports <TOTK_HOME>/totk as its executable. The game data goes there too: exefs/,
//   romfs/romfs.bin and profile/ (main.c portable_paths).
// - stdout and stderr, which go nowhere in an app, copied line by line to logcat under the tag "totk".
// - The arguments: SDLActivity passes what the launch intent's "args" extra holds (MainActivity.getArguments).
#ifdef __ANDROID__
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <android/log.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int totk_main(int argc, char **argv);

static int log_pipe[2];

static void *log_pump(void *arg) {
    (void)arg;
    char buf[4096];
    size_t have = 0;
    for (;;) {
        ssize_t n = read(log_pipe[0], buf + have, sizeof buf - 1 - have);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return NULL;
        }
        have += (size_t)n;
        char *line = buf, *nl;
        while ((nl = memchr(line, '\n', have - (size_t)(line - buf)))) {
            *nl = 0;
            __android_log_write(ANDROID_LOG_INFO, "totk", line);
            line = nl + 1;
        }
        have -= (size_t)(line - buf);
        memmove(buf, line, have);
        if (have == sizeof buf - 1) {   // a line longer than the buffer goes out in pieces
            buf[have] = 0;
            __android_log_write(ANDROID_LOG_INFO, "totk", buf);
            have = 0;
        }
    }
}

static void log_to_logcat(void) {
    if (pipe(log_pipe)) return;
    dup2(log_pipe[1], STDOUT_FILENO);
    dup2(log_pipe[1], STDERR_FILENO);
    pthread_t t;
    if (!pthread_create(&t, NULL, log_pump, NULL)) pthread_detach(t);
}

int SDL_main(int argc, char **argv) {
    log_to_logcat();
    const char *home = SDL_GetAndroidExternalStoragePath();
    if (!home) {
        __android_log_print(ANDROID_LOG_ERROR, "totk", "no external storage path: %s", SDL_GetError());
        return 1;
    }
    mkdir(home, 0770);
    setenv("TOTK_HOME", home, 1);
    if (chdir(home)) __android_log_print(ANDROID_LOG_WARN, "totk", "chdir %s: %s", home, strerror(errno));
    __android_log_print(ANDROID_LOG_INFO, "totk", "home %s, %d arguments", home, argc - 1);
    return totk_main(argc, argv);
}
#endif
