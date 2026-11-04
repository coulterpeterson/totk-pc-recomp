// Android entry point. SDLActivity (tools/android/) loads libtotk.so and calls SDL_main on a thread of its own; this
// file sets up what main() expects from a desktop process and then runs it (main.c names it totk_main on Android):
// - TOTK_HOME, the app's external files folder (/storage/emulated/0/Android/data/<package>/files). The runtime keeps
//   its files beside its executable (totk.ini, romfs-index.bin, shader caches, logs; plat_exe_path), and an app has
//   none, so platform/linux.c reports <TOTK_HOME>/totk as its executable. The game data goes there too: exefs/,
//   romfs/romfs.bin and profile/ (main.c portable_paths). profile/ is made here, as main.c takes it only when it is
//   there and would otherwise use the build machine's path.
// - stdout and stderr, which go nowhere in an app, copied line by line to logcat under the tag "totk". The copy runs
//   on a thread of its own, so plat_exit drains it (android_log_drain) before the process ends: a fatal error's last
//   lines are the ones that matter.
// - The arguments: SDLActivity passes what the launch intent's "args" extra holds (MainActivity.getArguments).
// - The shader translator: an app may run programs only from its native library folder, so the APK carries totk_xlate
//   as libtotk_xlate.so beside libtotk.so (tools/build_apk.py) and TOTK_XLATE names it; its identity
//   (tools/build_xlate.py totk_xlate.id, which tools/install_android.py puts into TOTK_HOME) becomes TOTK_XLATE_ID, so
//   shader caches translated elsewhere by the same sources keep their keys (gfx_shader.c xlate_identity).
// - A GPU driver of the player's: one Vulkan driver library in gpu_driver/ of the data folder (a Mesa Turnip build for
//   Adreno GPUs) is copied to the app's private storage, where a library may be loaded (shared storage may not be
//   mapped executable), and TOTK_VK_DRIVER names the copy (gfx_window_sdl.c driver_load). Removing the folder goes
//   back to the system's driver.
#ifdef __ANDROID__
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <android/log.h>
#include <jni.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "platform/platform.h"

int totk_main(int argc, char **argv);

static int log_pipe[2] = {-1, -1};
static pthread_t log_thread;

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
    if (pthread_create(&log_thread, NULL, log_pump, NULL)) log_pipe[0] = -1;
}

// Called by plat_exit before _exit (SDL_main ends through it too): once stdout, stderr and the pipe's own write end are
// closed, the pump reads what is left, sees the end of the pipe and returns.
void android_log_drain(void) {
    static int drained;
    if (log_pipe[0] < 0 || __atomic_exchange_n(&drained, 1, __ATOMIC_ACQ_REL)) return;
    fflush(stdout);
    fflush(stderr);
    int null = open("/dev/null", O_WRONLY);
    dup2(null, STDOUT_FILENO);
    dup2(null, STDERR_FILENO);
    close(null);
    close(log_pipe[1]);
    pthread_join(log_thread, NULL);
}

static void find_translator(const char *home) {
    Dl_info d;
    char path[1024];
    const char *slash;
    if (dladdr((void *)find_translator, &d) && d.dli_fname && (slash = strrchr(d.dli_fname, '/')) &&
        snprintf(path, sizeof path, "%.*s/libtotk_xlate.so", (int)(slash - d.dli_fname), d.dli_fname) < (int)sizeof path &&
        !access(path, X_OK))
        setenv("TOTK_XLATE", path, 0);
    else
        __android_log_print(ANDROID_LOG_WARN, "totk", "no runnable libtotk_xlate.so beside %s: shaders not in the cache "
                            "are not translated", d.dli_fname ? d.dli_fname : "libtotk.so");
    char id[128] = "";
    snprintf(path, sizeof path, "%s/totk_xlate.id", home);
    FILE *f = fopen(path, "r");
    if (f) {
        if (fgets(id, sizeof id, f)) id[strcspn(id, "\r\n")] = 0;
        fclose(f);
    }
    if (id[0]) setenv("TOTK_XLATE_ID", id, 0);
}

static int copy_file(const char *from, const char *to) {
    int in = open(from, O_RDONLY), out = in < 0 ? -1 : open(to, O_WRONLY | O_CREAT | O_TRUNC, 0700);
    char buf[1 << 16];
    ssize_t n = 0;
    while (out >= 0 && (n = read(in, buf, sizeof buf)) > 0)
        if (write(out, buf, (size_t)n) != n) n = -1;
    if (in >= 0) close(in);
    if (out >= 0) close(out);
    return in >= 0 && out >= 0 && n == 0;
}

static void find_gpu_driver(const char *home) {
    char dir[1024], name[256] = "";
    snprintf(dir, sizeof dir, "%s/gpu_driver", home);
    DIR *d = opendir(dir);
    if (!d) return;
    int found = 0;
    for (struct dirent *e; (e = readdir(d));) {
        size_t n = strlen(e->d_name);
        if (n > 3 && !strcmp(e->d_name + n - 3, ".so") && n < sizeof name) found++, strcpy(name, e->d_name);
    }
    closedir(d);
    if (found != 1) {
        if (found) __android_log_print(ANDROID_LOG_WARN, "totk", "%s holds %d .so files: no GPU driver taken", dir, found);
        return;
    }
    const char *priv = SDL_GetAndroidInternalStoragePath();
    char src[1400], dst[1400];
    struct stat a, b;
    snprintf(src, sizeof src, "%s/%s", dir, name);
    snprintf(dst, sizeof dst, "%s/gpu_driver", priv ? priv : "");
    mkdir(dst, 0700);
    snprintf(dst, sizeof dst, "%s/gpu_driver/%s", priv ? priv : "", name);
    if (!priv || stat(src, &a) || ((stat(dst, &b) || b.st_size != a.st_size || b.st_mtime < a.st_mtime) && !copy_file(src, dst))) {
        __android_log_print(ANDROID_LOG_ERROR, "totk", "GPU driver %s could not be copied to %s", src, dst);
        return;
    }
    setenv("TOTK_VK_DRIVER", dst, 0);
    __android_log_print(ANDROID_LOG_INFO, "totk", "GPU driver %s (copied from %s)", dst, src);
}

// Import save in the menu (MainActivity.java pickSave): the file browser, and how far the copy of its choice is.
void android_pick_save(const char *title, const char *zip, const char *folder) {
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    jobject act = (jobject)SDL_GetAndroidActivity();
    if (!env || !act) return;
    jclass c = (*env)->GetObjectClass(env, act);
    jmethodID m = (*env)->GetMethodID(env, c, "pickSave", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V");
    jstring t = (*env)->NewStringUTF(env, title), z = (*env)->NewStringUTF(env, zip), f = (*env)->NewStringUTF(env, folder);
    if (m) (*env)->CallVoidMethod(env, act, m, t, z, f);
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    (*env)->DeleteLocalRef(env, t), (*env)->DeleteLocalRef(env, z), (*env)->DeleteLocalRef(env, f);
    (*env)->DeleteLocalRef(env, c), (*env)->DeleteLocalRef(env, act);
}

int android_pick_save_state(void) {   // 0 idle, 1 choosing or copying, 2 copied, -1 failed; 2 and -1 are told once
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    jobject act = (jobject)SDL_GetAndroidActivity();
    if (!env || !act) return 0;
    jclass c = (*env)->GetObjectClass(env, act);
    jmethodID m = (*env)->GetMethodID(env, c, "pickSaveState", "()I");
    int s = m ? (*env)->CallIntMethod(env, act, m) : 0;
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env), s = 0;
    (*env)->DeleteLocalRef(env, c), (*env)->DeleteLocalRef(env, act);
    return s;
}

int SDL_main(int argc, char **argv) {
    log_to_logcat();
    const char *home = SDL_GetAndroidExternalStoragePath();
    if (!home) {
        __android_log_print(ANDROID_LOG_ERROR, "totk", "no external storage path: %s", SDL_GetError());
        return 1;
    }
    mkdir(home, 0770);
    char profile[1024];
    snprintf(profile, sizeof profile, "%s/profile", home);
    mkdir(profile, 0770);
    setenv("TOTK_HOME", home, 1);
    const char *cache = SDL_GetAndroidCachePath();   // Android has no /tmp: plat_temp_dir reads TMPDIR
    if (cache) setenv("TMPDIR", cache, 0);
    if (chdir(home)) __android_log_print(ANDROID_LOG_WARN, "totk", "chdir %s: %s", home, strerror(errno));
    find_translator(home);
    find_gpu_driver(home);
    __android_log_print(ANDROID_LOG_INFO, "totk", "home %s, %d arguments", home, argc - 1);
    int code = totk_main(argc, argv);
    // A desktop process ends when main returns and takes its threads with it. Returning to SDLActivity instead would
    // let it shut SDL down while the window thread still pumps events (a FORTIFY abort on a destroyed mutex).
    plat_exit(code);
}
#endif
