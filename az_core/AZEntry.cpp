#include <jni.h>
#include <mutex>
#include "Includes/Logger.h"

static JavaVM* g_azVm = nullptr;
static jobject g_azActivity = nullptr;
static jclass g_azBridgeClass = nullptr;
static std::mutex g_azJniMutex;

extern "C" __attribute__((visibility("default")))
JavaVM* AZ_GetJavaVM() {
    return g_azVm;
}

extern "C" __attribute__((visibility("default")))
jobject AZ_GetActivity() {
    return g_azActivity;
}

extern "C" __attribute__((visibility("default")))
jint B0(JNIEnv* env, jclass bridgeClass, jobject activity) {
    if (!env) return 0;

    std::lock_guard<std::mutex> lock(g_azJniMutex);
    env->GetJavaVM(&g_azVm);

    if (g_azActivity) {
        env->DeleteGlobalRef(g_azActivity);
        g_azActivity = nullptr;
    }
    if (g_azBridgeClass) {
        env->DeleteGlobalRef(g_azBridgeClass);
        g_azBridgeClass = nullptr;
    }

    if (activity) g_azActivity = env->NewGlobalRef(activity);
    if (bridgeClass) {
        g_azBridgeClass = static_cast<jclass>(env->NewGlobalRef(bridgeClass));
    }

    LOGI("[AZCORE] B0 ready vm=%p activity=%p bridge=%p",
         (void*)g_azVm, (void*)g_azActivity, (void*)g_azBridgeClass);
    return 1;
}

extern "C" __attribute__((visibility("default")))
jint JNI_OnLoad(JavaVM* vm, void*) {
    g_azVm = vm;
    LOGI("[AZCORE] JNI_OnLoad vm=%p", (void*)vm);
    return JNI_VERSION_1_6;
}

extern "C" __attribute__((visibility("default")))
const char* AZ_CoreVersion() {
    return "AZ Tool Core 0.2";
}
