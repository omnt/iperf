/*
 * JNI entry points used by OpenMobileNetworkToolkit (OMNT).
 *
 * The Java side declares, in
 *   de.fraunhofer.fokus.OpenMobileNetworkToolkit.Iperf3.Worker.Iperf3ExecutorWorker:
 *     private native int  iperf3Run(String[] argv);
 *     private native void iperf3Stop();
 *     // called from native code, once per iperf3 JSON stream event:
 *     private void onIperf3Event(byte[] utf8Json);
 *
 * argv is the iperf3 command line without the program name. It must
 * contain --json-stream. iperf3Run() runs one test through libiperf's API
 * (what upstream main() + run() do), delivers every JSON stream line
 * ("start", "interval", "end", "error", ...) to onIperf3Event() while the
 * test runs, and returns iperf3's exit code once it is finished.
 * iperf3Stop() asks a running test to finish early; a client still
 * delivers its final intervals and "end".
 *
 * Only one test can run per process at a time: libiperf keeps global
 * state (i_errno, getopt). OMNT runs every test in a worker process of
 * its own.
 */
#include <jni.h>
#include <errno.h>
#include <getopt.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <android/log.h>

#include "iperf_config.h"
#include "iperf.h"
#include "iperf_api.h"
#include "cjson.h"
#include "omnt_exit.h"

#define LOG_TAG "iperf3_jni"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

/* setjmp() cannot return 0 via longjmp(), so exit codes are offset. */
#define EXIT_JMP_BASE 0x100

/* ---------------------------------------------------------------------- */
/* State of the one test this process runs.                                */

static JavaVM *g_vm;
static jobject g_listener;          /* global ref to the Java worker */
static jmethodID g_on_event;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct iperf_test *g_test;   /* guarded by g_lock */
static volatile int g_stop_requested;

static jmp_buf g_exit_jmp;
static int g_exit_jmp_armed;        /* only touched by the test thread */
static pthread_t g_exit_jmp_thread;

/* ---------------------------------------------------------------------- */
/* IPERF_EXIT() target (see omnt_exit.h).                                  */

void
iperf_exit_jump(int code)
{
    if (g_exit_jmp_armed && pthread_equal(pthread_self(), g_exit_jmp_thread))
        longjmp(g_exit_jmp, EXIT_JMP_BASE + code);
    /* Not on the test thread (e.g. a stream thread): nothing to jump to. */
    LOGE("iperf3 exit(%d) outside of the test thread; terminating process", code);
    exit(code);
}

/* ---------------------------------------------------------------------- */
/* stdout/stderr -> logcat.                                                */

/*
 * Android discards stdout/stderr of app processes. iperf3 reports some
 * problems only there (usage, warnings), so forward both to logcat under
 * the tag "iperf3_stdio".
 */
static void *
stdio_to_logcat(void *arg)
{
    int fd = (int)(intptr_t)arg;
    char buf[1024];
    size_t len = 0;
    ssize_t n;
    while ((n = read(fd, buf + len, sizeof(buf) - 1 - len)) > 0) {
        len += (size_t)n;
        buf[len] = '\0';
        char *start = buf, *nl;
        while ((nl = strchr(start, '\n')) != NULL) {
            *nl = '\0';
            if (nl > start)
                __android_log_write(ANDROID_LOG_WARN, "iperf3_stdio", start);
            start = nl + 1;
        }
        len = strlen(start);
        memmove(buf, start, len + 1);
        if (len == sizeof(buf) - 1) { /* overlong line: flush it */
            __android_log_write(ANDROID_LOG_WARN, "iperf3_stdio", buf);
            len = 0;
        }
    }
    return NULL;
}

static void
redirect_stdio_once(void)
{
    static int done = 0;
    int pfd[2];
    pthread_t thr;
    if (done)
        return;
    done = 1;
    if (pipe(pfd) != 0) {
        LOGE("stdio redirect: pipe failed: %s", strerror(errno));
        return;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    dup2(pfd[1], STDOUT_FILENO);
    dup2(pfd[1], STDERR_FILENO);
    close(pfd[1]);
    if (pthread_create(&thr, NULL, stdio_to_logcat, (void *)(intptr_t)pfd[0]) == 0)
        pthread_detach(thr);
}

/* ---------------------------------------------------------------------- */
/* JSON stream -> Java.                                                    */

static void
deliver_to_java(const char *json)
{
    JNIEnv *env = NULL;
    int attached = 0;

    if (g_vm == NULL || g_listener == NULL || json == NULL)
        return;
    jint rc = (*g_vm)->GetEnv(g_vm, (void **)&env, JNI_VERSION_1_6);
    if (rc == JNI_EDETACHED) {
        /* iperf3 emits from the test thread; attach anyway, just in case. */
        if ((*g_vm)->AttachCurrentThread(g_vm, &env, NULL) != JNI_OK)
            return;
        attached = 1;
    } else if (rc != JNI_OK) {
        return;
    }

    /*
     * Pass raw UTF-8 bytes: NewStringUTF() expects *modified* UTF-8 and
     * aborts (CheckJNI) on e.g. 4-byte sequences in a --title.
     */
    jsize len = (jsize)strlen(json);
    jbyteArray bytes = (*env)->NewByteArray(env, len);
    if (bytes != NULL) {
        (*env)->SetByteArrayRegion(env, bytes, 0, len, (const jbyte *)json);
        (*env)->CallVoidMethod(env, g_listener, g_on_event, bytes);
        (*env)->DeleteLocalRef(env, bytes);
    }
    if ((*env)->ExceptionCheck(env)) {
        LOGE("onIperf3Event threw");
        (*env)->ExceptionDescribe(env);
        (*env)->ExceptionClear(env);
    }
    if (attached)
        (*g_vm)->DetachCurrentThread(g_vm);
}

/* Called by libiperf for every --json-stream line. */
static void
json_callback(struct iperf_test *test, char *json)
{
    (void)test;
    deliver_to_java(json);
}

/*
 * Emits {"event":"error","data":msg} in the same format libiperf's
 * JSONStream_Output() uses. Only needed where libiperf has no JSON output
 * set up yet (argument parsing), so the app still sees why the run failed.
 */
static void
emit_error_event(const char *msg)
{
    cJSON *event = cJSON_CreateObject();
    if (event == NULL)
        return;
    cJSON_AddStringToObject(event, "event", "error");
    cJSON_AddStringToObject(event, "data", msg);
    char *str = cJSON_PrintUnformatted(event);
    if (str != NULL) {
        deliver_to_java(str);
        cJSON_free(str);
    }
    cJSON_Delete(event);
}

/* ---------------------------------------------------------------------- */
/* Running a test.                                                         */

/*
 * Mirrors upstream main.c run() (iperf 3.22), minus daemon mode and the
 * SIGINT/SIGTERM/SIGHUP handlers: the app stops a test via iperf3Stop().
 * Returns < 0 if iperf_json_finish() failed, as run() does.
 */
static int
run_test(struct iperf_test *test)
{
    switch (test->role) {
        case 's':
            if (test->daemon)
                LOGE("daemon mode is not supported in-process; ignoring -D");
            if (iperf_create_pidfile(test) < 0) {
                i_errno = IEPIDFILE;
                iperf_errexit(test, "error - %s", iperf_strerror(i_errno));
            }
            for (;;) {
                int rc;
                rc = iperf_run_server(test);
                test->server_last_run_rc = rc;
                if (rc < 0) {
                    if (g_stop_requested)
                        break;
                    iperf_err(test, "error - %s", iperf_strerror(i_errno));
                    if (test->json_output) {
                        if (iperf_json_finish(test) < 0)
                            return -1;
                    }
                    iflush(test);

                    if (rc < -1) {
                        iperf_errexit(test, "exiting");
                    }
                }
                iperf_reset_test(test);
                if (g_stop_requested)
                    break;
                if (iperf_get_test_one_off(test) && rc != 2) {
                    /* Authentication failure doesn't count for 1-off test */
                    if (rc < 0 && i_errno == IEAUTHTEST) {
                        continue;
                    }
                    break;
                }
            }
            iperf_delete_pidfile(test);
            break;
        case 'c':
            if (iperf_create_pidfile(test) < 0) {
                i_errno = IEPIDFILE;
                iperf_errexit(test, "error - %s", iperf_strerror(i_errno));
            }
            if (iperf_run_client(test) < 0)
                iperf_errexit(test, "error - %s", iperf_strerror(i_errno));
            iperf_delete_pidfile(test);
            break;
        default:
            usage();
            break;
    }
    return 0;
}

static void
set_current_test(struct iperf_test *test)
{
    pthread_mutex_lock(&g_lock);
    g_test = test;
    pthread_mutex_unlock(&g_lock);
}

JNIEXPORT jint JNICALL
Java_de_fraunhofer_fokus_OpenMobileNetworkToolkit_Iperf3_Worker_Iperf3ExecutorWorker_iperf3Run(
    JNIEnv *env, jobject obj, jobjectArray jargv)
{
    /* volatile: modified between setjmp() and a possible longjmp(). */
    struct iperf_test *volatile test = NULL;
    char **volatile argv = NULL;
    volatile int argc = 0;
    volatile int result = 0;
    struct sigaction ign, old_pipe;

    redirect_stdio_once();

    pthread_mutex_lock(&g_lock);
    int busy = (g_test != NULL || g_listener != NULL);
    pthread_mutex_unlock(&g_lock);
    if (busy) {
        LOGE("an iperf3 test is already running in this process");
        return -1;
    }

    /* Java callback target. */
    (*env)->GetJavaVM(env, &g_vm);
    jclass cls = (*env)->GetObjectClass(env, obj);
    g_on_event = (*env)->GetMethodID(env, cls, "onIperf3Event", "([B)V");
    (*env)->DeleteLocalRef(env, cls);
    if (g_on_event == NULL) {
        LOGE("onIperf3Event([B)V not found");
        return -1; /* NoSuchMethodError is pending */
    }
    g_listener = (*env)->NewGlobalRef(env, obj);

    /* argv[0] is the program name for getopt. */
    argc = (*env)->GetArrayLength(env, jargv) + 1;
    argv = calloc((size_t)argc + 1, sizeof(char *));
    if (argv == NULL) {
        LOGE("out of memory");
        result = -1;
        goto out;
    }
    argv[0] = strdup("iperf3");
    for (int i = 1; i < argc; ++i) {
        jstring s = (jstring)(*env)->GetObjectArrayElement(env, jargv, i - 1);
        const char *c = s != NULL ? (*env)->GetStringUTFChars(env, s, NULL) : NULL;
        argv[i] = strdup(c != NULL ? c : "");
        if (c != NULL)
            (*env)->ReleaseStringUTFChars(env, s, c);
        if (s != NULL)
            (*env)->DeleteLocalRef(env, s);
        LOGD("argv[%d] = %s", i, argv[i]);
    }

    /*
     * libiperf keeps global state that the standalone binary never resets
     * because it runs once per process:
     *  - getopt_long()'s position (optind/optreset): without a reset every
     *    run after the first parses no options;
     *  - i_errno: iperf_parse_arguments() checks it after some options
     *    (--connect-timeout, -b, -w, ...), so a failed run made every
     *    following run fail with a parameter error.
     */
    optind = 1;
    optreset = 1;
    i_errno = 0;
    g_stop_requested = 0;

    /* Writing to a closed socket must fail with EPIPE, not kill the app. */
    memset(&ign, 0, sizeof(ign));
    ign.sa_handler = SIG_IGN;
    sigemptyset(&ign.sa_mask);
    sigaction(SIGPIPE, &ign, &old_pipe);

    test = iperf_new_test();
    if (test == NULL) {
        LOGE("iperf_new_test failed: %s", iperf_strerror(i_errno));
        emit_error_event("create new test error");
        result = 1;
        goto restore;
    }
    iperf_defaults(test);
    iperf_set_test_json_callback(test, json_callback);

    /* IPERF_EXIT() from parsing, the server loop or iperf_exit() lands here. */
    int jmp_rc = setjmp(g_exit_jmp);
    if (jmp_rc != 0) {
        g_exit_jmp_armed = 0;
        result = jmp_rc - EXIT_JMP_BASE;
        LOGD("iperf3 exited with code %d", result);
        goto free_test;
    }
    g_exit_jmp_thread = pthread_self();
    g_exit_jmp_armed = 1;

    if (iperf_parse_arguments(test, argc, argv) < 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "parameter error - %s", iperf_strerror(i_errno));
        LOGE("%s", msg);
        emit_error_event(msg);
        g_exit_jmp_armed = 0;
        result = 1;
        goto free_test;
    }
    /* The json callback is the only output path the app reads. */
    if (!test->json_stream)
        LOGE("argv has no --json-stream; the app will not receive events");

    set_current_test(test);
    if (run_test(test) < 0)
        iperf_errexit(test, "error - %s", iperf_strerror(i_errno));
    g_exit_jmp_armed = 0;
    result = 0;

free_test:
    set_current_test(NULL);
    fflush(stdout);
    fflush(stderr);
    iperf_free_test(test);
    test = NULL;
restore:
    sigaction(SIGPIPE, &old_pipe, NULL);
out:
    if (argv != NULL) {
        for (int i = 0; i < argc; ++i)
            free(argv[i]);
        free(argv);
    }
    pthread_mutex_lock(&g_lock);
    (*env)->DeleteGlobalRef(env, g_listener);
    g_listener = NULL;
    pthread_mutex_unlock(&g_lock);
    LOGD("result: %d", result);
    return result;
}

JNIEXPORT void JNICALL
Java_de_fraunhofer_fokus_OpenMobileNetworkToolkit_Iperf3_Worker_Iperf3ExecutorWorker_iperf3Stop(
    JNIEnv *env, jobject obj)
{
    (void)env;
    (void)obj;
    LOGD("iperf3Stop() called");
    pthread_mutex_lock(&g_lock);
    g_stop_requested = 1;
    if (g_test != NULL) {
        /* A client ends the test at its next timer tick and reports "end". */
        g_test->done = 1;
        /*
         * A server may sit in select()/accept() waiting for a client;
         * shutting the listening socket down wakes it up.
         */
        if (g_test->role == 's' && g_test->listener >= 0)
            shutdown(g_test->listener, SHUT_RDWR);
    }
    pthread_mutex_unlock(&g_lock);
}
