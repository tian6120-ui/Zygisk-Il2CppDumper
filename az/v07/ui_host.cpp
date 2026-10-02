#include <jni.h>
#include <thread>
#include <atomic>
#include <unistd.h>
#include <sys/stat.h>
#include <cstdio>
#include <cstring>
#include <string>
#include "game_ui.h"

static std::atomic<bool> started{false};

static std::string pkg(){
 FILE*f=fopen("/proc/self/cmdline","rb");if(!f)return"unknown";
 char b[256]{};size_t n=fread(b,1,sizeof(b)-1,f);fclose(f);
 std::string s(b,n);auto z=s.find('\0');if(z!=std::string::npos)s.resize(z);
 auto c=s.find(':');if(c!=std::string::npos)s.resize(c);return s.empty()?"unknown":s;
}
static void worker(){
 std::string p="/data/user/0/"+pkg()+"/files/AZTool/agent.ready";
 for(int i=0;i<360;++i){if(access(p.c_str(),F_OK)==0){az_ui_activate();return;}usleep(250000);}
}
static void start(){bool e=false;if(started.compare_exchange_strong(e,true))std::thread(worker).detach();}

extern "C" __attribute__((visibility("default")))
void SetTargetActivity(JNIEnv* env,jobject ctx){az_ui_set_context(env,ctx);start();}
extern "C" __attribute__((visibility("default")))
jint JNI_OnLoad(JavaVM* vm,void*){az_ui_set_vm(vm);start();return JNI_VERSION_1_6;}
__attribute__((constructor)) static void init(){start();}
