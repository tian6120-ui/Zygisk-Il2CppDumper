#pragma once
#include <jni.h>
#include <stdint.h>
namespace zygisk {
struct Api; struct AppSpecializeArgs; struct ServerSpecializeArgs;
class ModuleBase { public: virtual ~ModuleBase()=default; virtual void onLoad(Api*,JNIEnv*){} virtual void preAppSpecialize(AppSpecializeArgs*){} virtual void postAppSpecialize(const AppSpecializeArgs*){} virtual void preServerSpecialize(ServerSpecializeArgs*){} virtual void postServerSpecialize(const ServerSpecializeArgs*){} };
struct AppSpecializeArgs { jint&uid; jint&gid; jintArray&gids; jint&runtime_flags; jobjectArray&rlimits; jint&mount_external; jstring&se_info; jstring&nice_name; jstring&instruction_set; jstring&app_data_dir; jintArray*const fds_to_ignore; jboolean*const is_child_zygote; jboolean*const is_top_app; jobjectArray*const pkg_data_info_list; jobjectArray*const whitelisted_data_info_list; jboolean*const mount_data_dirs; jboolean*const mount_storage_dirs; jboolean*const mount_sysprop_overrides; AppSpecializeArgs()=delete; };
struct ServerSpecializeArgs { jint&uid; jint&gid; jintArray&gids; jint&runtime_flags; jlong&permitted_capabilities; jlong&effective_capabilities; ServerSpecializeArgs()=delete; };
namespace internal { struct api_table; template<class T> void entry_impl(api_table*,JNIEnv*); }
enum Option:int { FORCE_DENYLIST_UNMOUNT=0, DLCLOSE_MODULE_LIBRARY=1 };
struct Api { int connectCompanion(); int getModuleDir(); void setOption(Option); uint32_t getFlags(); bool exemptFd(int); void hookJniNativeMethods(JNIEnv*,const char*,JNINativeMethod*,int); private: internal::api_table*tbl{}; template<class T> friend void internal::entry_impl(internal::api_table*,JNIEnv*); };
namespace internal {
struct module_abi { long api_version; ModuleBase*impl; void(*preAppSpecialize)(ModuleBase*,AppSpecializeArgs*); void(*postAppSpecialize)(ModuleBase*,const AppSpecializeArgs*); void(*preServerSpecialize)(ModuleBase*,ServerSpecializeArgs*); void(*postServerSpecialize)(ModuleBase*,const ServerSpecializeArgs*); explicit module_abi(ModuleBase*m):api_version(5),impl(m){preAppSpecialize=[](auto m,auto a){m->preAppSpecialize(a);};postAppSpecialize=[](auto m,auto a){m->postAppSpecialize(a);};preServerSpecialize=[](auto m,auto a){m->preServerSpecialize(a);};postServerSpecialize=[](auto m,auto a){m->postServerSpecialize(a);};}};
struct api_table { void*impl; bool(*registerModule)(api_table*,module_abi*); void(*hookJniNativeMethods)(JNIEnv*,const char*,JNINativeMethod*,int); void(*pltHookRegister)(unsigned long,unsigned long,const char*,void*,void**); bool(*exemptFd)(int); bool(*pltHookCommit)(); int(*connectCompanion)(void*); void(*setOption)(void*,Option); int(*getModuleDir)(void*); uint32_t(*getFlags)(void*); };
template<class T> void entry_impl(api_table*t,JNIEnv*e){static Api a;a.tbl=t;static T mod;static module_abi abi(&mod);if(!t->registerModule(t,&abi))return;mod.onLoad(&a,e);}
}
inline int Api::getModuleDir(){return tbl&&tbl->getModuleDir?tbl->getModuleDir(tbl->impl):-1;}
inline void Api::setOption(Option o){if(tbl&&tbl->setOption)tbl->setOption(tbl->impl,o);}
inline bool Api::exemptFd(int fd){return tbl&&tbl->exemptFd&&tbl->exemptFd(fd);}
}
#define REGISTER_ZYGISK_MODULE(clazz) extern "C" __attribute__((visibility("default"))) void zygisk_module_entry(zygisk::internal::api_table*t,JNIEnv*e){zygisk::internal::entry_impl<clazz>(t,e);} extern "C" __attribute__((visibility("default"))) void zygisk_companion_entry(int){}
