#pragma once
#include <jni.h>
extern "C" void az_ui_set_vm(JavaVM* vm);
extern "C" void az_ui_set_context(JNIEnv* env, jobject context);
extern "C" void az_ui_activate();
