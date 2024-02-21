#include <jni.h>
#include <stdio.h>
#include <string.h>
#include <android/log.h>
#include <stdlib.h>

#include "de_fraunhofer_fokus_OpenMobileNetworkToolkit_Iperf3Worker.h"
#include "main.h"

#define  LOG_TAG    "iperf3Wrapper_native_Worker"
#define  LOGD(...)  __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define  LOGE(...)  __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define BUF_SIZE 4096

JNIEXPORT jint JNICALL Java_de_fraunhofer_fokus_OpenMobileNetworkToolkit_Iperf3_Iperf3Worker_iperf3Wrapper(JNIEnv *env, jobject obj, jobjectArray stringArray, jstring cache_path) {
    jsize ArgCount = (*env)->GetArrayLength(env, stringArray) + 1;
    char **argv = malloc(sizeof(char*) * ArgCount);
    if (argv == NULL) {
        LOGE("Failed to allocate memory for argv.");
        return -1;
    }

    argv[0] = LOG_TAG;
    LOGD("argc: %d\n", ArgCount);

    char* cache_path_native = (*env)->GetStringUTFChars(env, cache_path, 0);
    if (cache_path_native == NULL) {
        LOGE("Failed to obtain cache path.");
        free(argv);
        return -1;
    }

    LOGD("path: %s\n", cache_path_native);

    char buf[BUF_SIZE];
    snprintf(buf, BUF_SIZE, "TMPDIR=%s", cache_path_native);
    LOGD("complete path: %s\n", buf);

    int i;
    for (i = 1; i < ArgCount; ++i) {
        jstring string = (jstring)((*env)->GetObjectArrayElement(env, stringArray, i - 1));
        if (string != NULL) {
            const char *cstring = (*env)->GetStringUTFChars(env, string, 0);
            if (cstring != NULL) {
                argv[i] = strdup(cstring);
                (*env)->ReleaseStringUTFChars(env, string, cstring);
                (*env)->DeleteLocalRef(env, string);
                LOGD("argv[%d] = %s\n", i, argv[i]);
            } else {
                LOGE("Failed to obtain argument string.");
            }
        } else {
            LOGE("Failed to obtain object element.");
        }
    }

    int result = main(ArgCount, argv);
    LOGD("result: %d\n", result);

    for (i = 1; i < ArgCount; ++i) {
        free(argv[i]);
    }
    free(argv);
    (*env)->ReleaseStringUTFChars(env, cache_path, cache_path_native);

    return result;
}

JNIEXPORT jint JNICALL Java_de_fraunhofer_fokus_OpenMobileNetworkToolkit_Iperf3_Iperf3Worker_iperf3Stop(JNIEnv *env, jobject obj) {
    LOGD("stop function called!\n");
    stopRun();
    LOGD("exiting....!\n");
    return 0;
}

