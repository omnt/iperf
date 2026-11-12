/*
 * JNI entry points used by OpenMobileNetworkToolkit (OMNT).
 *
 * The Java side declares, in
 *   de.fraunhofer.fokus.OpenMobileNetworkToolkit.Iperf3.Worker.Iperf3ExecutorWorker:
 *     private native int iperf3Wrapper(String[] argv, String cache);
 *
 * argv is the iperf3 command line without the program name. The call
 * runs iperf3's main() in-process and returns its exit code.
 */
#include <jni.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <android/log.h>

#include "main.h"

#define LOG_TAG "iperf3Wrapper_native_Worker"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

JNIEXPORT jint JNICALL
Java_de_fraunhofer_fokus_OpenMobileNetworkToolkit_Iperf3_Worker_Iperf3ExecutorWorker_iperf3Wrapper(
    JNIEnv *env, jobject obj, jobjectArray stringArray, jstring cache_path)
{
    (void)obj;
    jsize argc = (*env)->GetArrayLength(env, stringArray) + 1;
    char **argv = calloc((size_t)argc + 1, sizeof(char *));
    if (argv == NULL) {
        LOGE("Failed to allocate memory for argv.");
        return -1;
    }

    argv[0] = strdup(LOG_TAG);
    LOGD("argc: %d", argc);

    const char *cache_path_native = (*env)->GetStringUTFChars(env, cache_path, 0);
    if (cache_path_native == NULL) {
        LOGE("Failed to obtain cache path.");
        free(argv[0]);
        free(argv);
        return -1;
    }
    LOGD("path: %s", cache_path_native);

    for (jsize i = 1; i < argc; ++i) {
        jstring string = (jstring)(*env)->GetObjectArrayElement(env, stringArray, i - 1);
        if (string == NULL) {
            LOGE("Failed to obtain object element.");
            argv[i] = strdup("");
            continue;
        }
        const char *cstring = (*env)->GetStringUTFChars(env, string, 0);
        if (cstring != NULL) {
            argv[i] = strdup(cstring);
            (*env)->ReleaseStringUTFChars(env, string, cstring);
            LOGD("argv[%d] = %s", i, argv[i]);
        } else {
            LOGE("Failed to obtain argument string.");
            argv[i] = strdup("");
        }
        (*env)->DeleteLocalRef(env, string);
    }

    int result = main(argc, argv);
    LOGD("result: %d", result);

    for (jsize i = 0; i < argc; ++i)
        free(argv[i]);
    free(argv);
    (*env)->ReleaseStringUTFChars(env, cache_path, cache_path_native);

    return result;
}

JNIEXPORT jint JNICALL
Java_de_fraunhofer_fokus_OpenMobileNetworkToolkit_Iperf3_Iperf3Worker_iperf3Stop(JNIEnv *env, jobject obj)
{
    (void)env;
    (void)obj;
    LOGD("stop function called!");
    stopRun();
    return 0;
}
