#include <jni.h>
#include <unistd.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <pthread.h>
#include <cstring>
#include <string>
#include <atomic>
#include "zygisk.hpp"

using zygisk::Api;
using zygisk::AppSpecializeArgs;

namespace {
Api* g_api=nullptr;
JavaVM* g_vm=nullptr;
bool g_active=false;
bool g_translated=false;
int g_core_fd=-1;
int g_ui_fd=-1;
void* g_native_core=nullptr;
void* g_host_ui=nullptr;

static bool has(const char*s,const char*q){return s&&q&&strstr(s,q);}
static bool mainUserApp(JNIEnv*e,AppSpecializeArgs*a){
 if(!a||!a->nice_name||!a->app_data_dir)return false;
 if((int)a->uid<10000)return false;
 const char*n=e->GetStringUTFChars(a->nice_name,nullptr);if(!n)return false;
 bool ok=strchr(n,':')==nullptr && strcmp(n,"zygote")!=0 && strcmp(n,"zygote64")!=0;
 e->ReleaseStringUTFChars(a->nice_name,n);return ok;
}
static std::string isaOf(JNIEnv*e,AppSpecializeArgs*a){
 if(!a||!a->instruction_set)return{};
 const char*s=e->GetStringUTFChars(a->instruction_set,nullptr);std::string r=s?s:"";if(s)e->ReleaseStringUTFChars(a->instruction_set,s);return r;
}
static void callJniOnLoad(void*h){
 if(!h||!g_vm)return;using F=jint(*)(JavaVM*,void*);auto f=(F)dlsym(h,"JNI_OnLoad");if(f)f(g_vm,nullptr);
}
static void callContext(void*h,JNIEnv*e,jobject ctx){
 if(!h||!e||!ctx)return;using F=void(*)(JNIEnv*,jobject);auto f=(F)dlsym(h,"SetTargetActivity");if(f)f(e,ctx);
}
static void* contextWorker(void*){
 if(!g_vm)return nullptr;JNIEnv*e=nullptr;if(g_vm->AttachCurrentThread(&e,nullptr)!=JNI_OK)return nullptr;
 jobject app=nullptr;
 for(int i=0;i<150&&!app;i++){
   jclass at=e->FindClass("android/app/ActivityThread");
   if(at){
     jmethodID m=e->GetStaticMethodID(at,"currentApplication","()Landroid/app/Application;");
     if(m)app=e->CallStaticObjectMethod(at,m);
     if(e->ExceptionCheck())e->ExceptionClear();
     e->DeleteLocalRef(at);
   }
   if(!app)usleep(200000);
 }
 if(app){callContext(g_native_core,e,app);callContext(g_host_ui,e,app);e->DeleteLocalRef(app);}
 g_vm->DetachCurrentThread();return nullptr;
}

#if defined(__i386__) || defined(__x86_64__)
typedef void* (*NBLoadLibraryFn)(const char*,int);
typedef bool (*NBIsPathSupportedFn)(const char*);
typedef const char* (*NBGetErrorFn)();
typedef void* (*NBLoadLibraryExtFn)(const char*,int,void*);
typedef void* (*NBGetExportedNamespaceFn)(const char*);
struct NBCb {
 uint32_t version;void* initialize;NBLoadLibraryFn loadLibrary;void* getTrampoline;void* isSupported;void* getAppEnv;void* isCompatibleWith;
 void* getSignalHandler;void* unloadLibrary;NBGetErrorFn getError;NBIsPathSupportedFn isPathSupported;void* initAnonymousNamespace;void* createNamespace;
 void* linkNamespaces;NBLoadLibraryExtFn loadLibraryExt;void* getVendorNamespace;NBGetExportedNamespaceFn getExportedNamespace;
};
static void* nbLoad(const char*path){
 const char*libs[]={"libhoudini.so","libndk_translation.so","libndk_translation_proxy_lib.so",nullptr};
 for(int i=0;libs[i];i++){
   void*t=dlopen(libs[i],RTLD_NOW|RTLD_LOCAL);if(!t)continue;
   auto*cb=(NBCb*)dlsym(t,"NativeBridgeItf");if(!cb)continue;
   if(cb->version>=3&&cb->loadLibraryExt){
     void*h=cb->loadLibraryExt(path,RTLD_NOW,(void*)3);if(h)return h;
     if(cb->version>=5&&cb->getExportedNamespace){void*ns=cb->getExportedNamespace("default");if(ns){h=cb->loadLibraryExt(path,RTLD_NOW,ns);if(h)return h;}}
     h=cb->loadLibraryExt(path,RTLD_NOW,nullptr);if(h)return h;
   }
   if(cb->loadLibrary){void*h=cb->loadLibrary(path,RTLD_NOW);if(h)return h;}
 }
 return nullptr;
}
#endif

static void* loadWorker(void*){
 if(g_ui_fd>=0){
   char p[64];snprintf(p,sizeof(p),"/proc/self/fd/%d",g_ui_fd);
   g_host_ui=dlopen(p,RTLD_NOW|RTLD_LOCAL);close(g_ui_fd);g_ui_fd=-1;
   if(g_host_ui)callJniOnLoad(g_host_ui);
 }
 if(g_core_fd>=0){
   char p[64];snprintf(p,sizeof(p),"/proc/self/fd/%d",g_core_fd);
#if defined(__i386__) || defined(__x86_64__)
   if(g_translated){(void)nbLoad(p);}
   else
#endif
   {g_native_core=dlopen(p,RTLD_NOW|RTLD_LOCAL);if(g_native_core)callJniOnLoad(g_native_core);}
   close(g_core_fd);g_core_fd=-1;
 }
 pthread_t t{};if(pthread_create(&t,nullptr,contextWorker,nullptr)==0)pthread_detach(t);
 return nullptr;
}
}

class AZ07Module final:public zygisk::ModuleBase{
 JNIEnv* env_{};
 public:
 void onLoad(Api*api,JNIEnv*env)override{g_api=api;env_=env;if(env)env->GetJavaVM(&g_vm);}
 void preAppSpecialize(AppSpecializeArgs*a)override{
   if(!mainUserApp(env_,a)){g_api->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);return;}
   int dir=g_api->getModuleDir();if(dir<0)return;
   std::string isa=isaOf(env_,a);
   const char*core=nullptr;const char*ui=nullptr;
#if defined(__aarch64__)
   core="payload.arm64";
#elif defined(__arm__)
   core="payload.arm32";
#elif defined(__x86_64__)
   if(has(isa.c_str(),"arm64")||has(isa.c_str(),"aarch64")){core="payload.arm64";ui="hostui.x86_64";g_translated=true;}
   else if(has(isa.c_str(),"arm")){core="payload.arm32";ui="hostui.x86_64";g_translated=true;}
   else core="payload.x86_64";
#elif defined(__i386__)
   if(has(isa.c_str(),"arm")){core="payload.arm32";ui="hostui.x86";g_translated=true;}
   else core="payload.x86";
#endif
   if(core){g_core_fd=openat(dir,core,O_RDONLY);if(g_core_fd>=0)g_api->exemptFd(g_core_fd);}
   if(ui){g_ui_fd=openat(dir,ui,O_RDONLY);if(g_ui_fd>=0)g_api->exemptFd(g_ui_fd);}
   close(dir);g_active=g_core_fd>=0;
 }
 void postAppSpecialize(const AppSpecializeArgs*)override{
   if(!g_active)return;pthread_t t{};if(pthread_create(&t,nullptr,loadWorker,nullptr)==0)pthread_detach(t);
 }
};
REGISTER_ZYGISK_MODULE(AZ07Module)
