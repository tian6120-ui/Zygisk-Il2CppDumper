#pragma once
#include <jni.h>
namespace AZOverlay {
bool init(JavaVM* vm, JNIEnv* env);
void shutdown();
}
