#include <jni.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

static void close_pair(int pair[2]) { close(pair[0]); close(pair[1]); }

JNIEXPORT jintArray JNICALL
Java_com_jarves_mh_runtime_NativeSpawn_spawn(JNIEnv *env, jobject self, jobjectArray java_argv,
                                               jobjectArray java_env, jstring java_cwd,
                                               jstring java_output) {
    (void)self;
    jsize argc = (*env)->GetArrayLength(env, java_argv);
    jsize envc = (*env)->GetArrayLength(env, java_env);
    char **argv = calloc((size_t)argc + 1, sizeof(char *));
    char **envp = calloc((size_t)envc + 1, sizeof(char *));
    if (!argv || !envp) return NULL;
    for (jsize i = 0; i < argc; i++) {
        jstring value = (jstring)(*env)->GetObjectArrayElement(env, java_argv, i);
        const char *utf = (*env)->GetStringUTFChars(env, value, NULL);
        argv[i] = strdup(utf);
        (*env)->ReleaseStringUTFChars(env, value, utf);
        (*env)->DeleteLocalRef(env, value);
    }
    for (jsize i = 0; i < envc; i++) {
        jstring value = (jstring)(*env)->GetObjectArrayElement(env, java_env, i);
        const char *utf = (*env)->GetStringUTFChars(env, value, NULL);
        envp[i] = strdup(utf);
        (*env)->ReleaseStringUTFChars(env, value, utf);
        (*env)->DeleteLocalRef(env, value);
    }
    const char *cwd_utf = (*env)->GetStringUTFChars(env, java_cwd, NULL);
    char *cwd = strdup(cwd_utf);
    (*env)->ReleaseStringUTFChars(env, java_cwd, cwd_utf);
    const char *output_utf = (*env)->GetStringUTFChars(env, java_output, NULL);
    char *output_path = strdup(output_utf);
    (*env)->ReleaseStringUTFChars(env, java_output, output_utf);

    int in_pipe[2];
    if (pipe(in_pipe) != 0) return NULL;
    pid_t pid = fork();
    if (pid == 0) {
        // Give every runtime launch its own process group so stopping the wrapper
        // also stops Claude Code and commands spawned underneath it.
        setpgid(0, 0);
        close(in_pipe[1]);
        int output_fd = open(output_path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
        if (output_fd < 0) _exit(126);
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(output_fd, STDOUT_FILENO);
        dup2(output_fd, STDERR_FILENO);
        close(in_pipe[0]);
        close(output_fd);
        chdir(cwd);
        prctl(PR_SET_DUMPABLE, 1, 0, 0, 0);
        execve(argv[0], argv, envp);
        dprintf(STDERR_FILENO, "Pocket native exec failed: %s\n", strerror(errno));
        _exit(127);
    }
    if (pid > 0) setpgid(pid, pid);
    close(in_pipe[0]);
    for (jsize i = 0; i < argc; i++) free(argv[i]);
    for (jsize i = 0; i < envc; i++) free(envp[i]);
    free(argv); free(envp); free(cwd); free(output_path);
    if (pid < 0) { close_pair(in_pipe); return NULL; }
    jint values[2] = {pid, in_pipe[1]};
    jintArray result = (*env)->NewIntArray(env, 2);
    (*env)->SetIntArrayRegion(env, result, 0, 2, values);
    return result;
}

JNIEXPORT jint JNICALL
Java_com_jarves_mh_runtime_NativeSpawn_waitFor(JNIEnv *env, jobject self, jint pid, jboolean no_hang) {
    (void)env; (void)self;
    int status = 0;
    pid_t value = waitpid(pid, &status, no_hang ? WNOHANG : 0);
    if (value == 0) return -2;
    if (value < 0) return -128 - errno;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}

JNIEXPORT jintArray JNICALL
Java_com_jarves_mh_runtime_NativeSpawn_spawnPty(JNIEnv *env, jobject self, jobjectArray java_argv,
                                                  jobjectArray java_env, jstring java_cwd) {
    (void)self;
    jsize argc = (*env)->GetArrayLength(env, java_argv);
    jsize envc = (*env)->GetArrayLength(env, java_env);
    char **argv = calloc((size_t)argc + 1, sizeof(char *));
    char **envp = calloc((size_t)envc + 1, sizeof(char *));
    if (!argv || !envp) return NULL;
    for (jsize i = 0; i < argc; i++) {
        jstring v = (jstring)(*env)->GetObjectArrayElement(env, java_argv, i);
        const char *u = (*env)->GetStringUTFChars(env, v, NULL);
        argv[i] = strdup(u);
        (*env)->ReleaseStringUTFChars(env, v, u);
        (*env)->DeleteLocalRef(env, v);
    }
    for (jsize i = 0; i < envc; i++) {
        jstring v = (jstring)(*env)->GetObjectArrayElement(env, java_env, i);
        const char *u = (*env)->GetStringUTFChars(env, v, NULL);
        envp[i] = strdup(u);
        (*env)->ReleaseStringUTFChars(env, v, u);
        (*env)->DeleteLocalRef(env, v);
    }
    const char *cwd_utf = (*env)->GetStringUTFChars(env, java_cwd, NULL);
    char *cwd = strdup(cwd_utf);
    (*env)->ReleaseStringUTFChars(env, java_cwd, cwd_utf);

    int master_fd = posix_openpt(O_RDWR | O_NOCTTY);
    if (master_fd < 0) { free(argv); free(envp); free(cwd); return NULL; }
    if (grantpt(master_fd) != 0 || unlockpt(master_fd) != 0) {
        close(master_fd); free(argv); free(envp); free(cwd); return NULL;
    }
    char slave_name[256];
    if (ptsname_r(master_fd, slave_name, sizeof(slave_name)) != 0) {
        close(master_fd); free(argv); free(envp); free(cwd); return NULL;
    }

    /* Set a sane window size (80x24) on the master before forking */
    struct winsize ws = { .ws_row = 24, .ws_col = 80 };
    ioctl(master_fd, TIOCSWINSZ, &ws);

    pid_t pid = fork();
    if (pid == 0) {
        close(master_fd);
        setsid();
        int slave_fd = open(slave_name, O_RDWR);
        if (slave_fd < 0) _exit(126);
        ioctl(slave_fd, TIOCSCTTY, 0);
        dup2(slave_fd, STDIN_FILENO);
        dup2(slave_fd, STDOUT_FILENO);
        dup2(slave_fd, STDERR_FILENO);
        if (slave_fd > STDERR_FILENO) close(slave_fd);
        setpgid(0, 0);
        chdir(cwd);
        prctl(PR_SET_DUMPABLE, 1, 0, 0, 0);
        setenv("TERM", "xterm-256color", 1);
        execve(argv[0], argv, envp);
        _exit(127);
    }
    if (pid > 0) setpgid(pid, pid);
    for (jsize i = 0; i < argc; i++) free(argv[i]);
    for (jsize i = 0; i < envc; i++) free(envp[i]);
    free(argv); free(envp); free(cwd);
    if (pid < 0) { close(master_fd); return NULL; }
    jint values[2] = {(jint)pid, (jint)master_fd};
    jintArray result = (*env)->NewIntArray(env, 2);
    (*env)->SetIntArrayRegion(env, result, 0, 2, values);
    return result;
}

JNIEXPORT jint JNICALL
Java_com_jarves_mh_runtime_NativeSpawn_writePty(JNIEnv *env, jobject self, jint master_fd, jbyteArray data) {
    (void)self;
    jsize len = (*env)->GetArrayLength(env, data);
    jbyte *buf = (*env)->GetByteArrayElements(env, data, NULL);
    jint written = (jint)write((int)master_fd, buf, (size_t)len);
    (*env)->ReleaseByteArrayElements(env, data, buf, JNI_ABORT);
    return written;
}

JNIEXPORT jint JNICALL
Java_com_jarves_mh_runtime_NativeSpawn_kill(JNIEnv *env, jobject self, jint pid, jint signal) {
    (void)env; (void)self;
    // Negative pid targets the whole runtime process group. Fall back to the
    // wrapper pid for devices where group creation raced with an early exit.
    int result = kill(-pid, signal);
    if (result != 0 && errno == ESRCH) result = kill(pid, signal);
    return result;
}
