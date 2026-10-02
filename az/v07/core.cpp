#include <jni.h>
#include <atomic>
#include <thread>
#include <unistd.h>
#include <sys/stat.h>
#include <cstdio>
#include <cstring>
#include <string>
#include "Il2cpp/Il2cpp.h"
#include "Includes/Logger.h"
#include "game_ui.h"

extern "C" void az_lua_worker();

static std::atomic<bool> g_started{false};

static bool mapped(const char *needle){
 FILE*f=fopen("/proc/self/maps","r"); if(!f) return false;
 char line[1024]; bool ok=false;
 while(fgets(line,sizeof(line),f)){ if(strstr(line,needle)){ok=true;break;} }
 fclose(f); return ok;
}

static void mark(const char *name,const char *text){
 std::string pkg;
 FILE*f=fopen("/proc/self/cmdline","rb");
 if(f){char b[256]{};size_t n=fread(b,1,sizeof(b)-1,f);fclose(f);pkg.assign(b,n);auto z=pkg.find('\0');if(z!=std::string::npos)pkg.resize(z);auto c=pkg.find(':');if(c!=std::string::npos)pkg.resize(c);}
 if(pkg.empty()) return;
 std::string d="/data/user/0/"+pkg+"/files/AZTool"; mkdir(d.c_str(),0700);
 std::string p=d+"/"+name; FILE*o=fopen(p.c_str(),"wb"); if(o){fwrite(text,1,strlen(text),o);fclose(o);}
}

static void worker(){
 mark("script.status","WAIT_IL2CPP");
 for(int i=0;i<240 && !mapped("libil2cpp.so");++i) usleep(250000);
 if(!mapped("libil2cpp.so")){mark("script.status","NO_IL2CPP");return;}
 bool ok=false;
 for(int i=0;i<240 && !ok;++i){ok=Il2cpp::Init() && Il2cpp::EnsureAttached();if(!ok)usleep(250000);}
 if(!ok){mark("script.status","IL2CPP_INIT_FAILED");return;}
 mark("agent.ready","AZ ScriptCore 0.7");
 mark("script.status","READY");
 az_ui_activate();
 az_lua_worker();
}

static void start_once(){
 bool expected=false;
 if(g_started.compare_exchange_strong(expected,true)){
   std::thread(worker).detach();
 }
}

extern "C" __attribute__((visibility("default")))
void SetTargetActivity(JNIEnv* env, jobject ctx){ az_ui_set_context(env,ctx); start_once(); }

extern "C" __attribute__((visibility("default")))
jint JNI_OnLoad(JavaVM* vm, void*){ az_ui_set_vm(vm); start_once(); return JNI_VERSION_1_6; }

__attribute__((constructor))
static void az_ctor(){ start_once(); }
