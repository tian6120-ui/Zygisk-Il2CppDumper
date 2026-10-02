#include <android/log.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <chrono>
#include <sys/syscall.h>
#include "Il2cpp/Il2cpp.h"
#include "Il2cpp/il2cpp-class.h"
#include "Dobby/dobby.h"
#include "Includes/Logger.h"
extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

namespace AZLua {
static std::string g_pkg, g_base, g_req, g_out, g_status;
static std::string procPackage(){
 FILE*f=fopen("/proc/self/cmdline","rb"); if(!f)return "unknown";
 char b[256]{}; size_t n=fread(b,1,sizeof(b)-1,f); fclose(f);
 std::string s(b,n); auto z=s.find('\0'); if(z!=std::string::npos)s.resize(z);
 auto colon=s.find(':'); if(colon!=std::string::npos)s.resize(colon);
 return s.empty()?"unknown":s;
}
static const char* BASE(){return g_base.c_str();}
static const char* REQ(){return g_req.c_str();}
static const char* OUT(){return g_out.c_str();}
static const char* STATUS(){return g_status.c_str();}
struct Obj { Il2CppObject *raw{}; uint32_t handle{}; };
struct Cls { Il2CppClass *klass{}; };
struct HookRec { int id{}; MethodInfo *method{}; void *target{}; void *orig{}; };
static lua_State *G=nullptr;
static std::atomic<bool> g_stopRequested{false};
static std::mutex g_runMetaMu;
static std::string g_runningName;
static long long g_runningStartMs=0;
static std::mutex hookMu;
static std::unordered_map<int,HookRec> hooks;
static std::atomic<int> nextHook{1};

// Match SilverV7's Lua-object ownership model:
// ordinary userdata stores raw Il2CppObject* with handle=0.
// Call-return references may be rooted externally instead of making the
// userdata own/free the handle during Lua collectgarbage().
static std::mutex g_rootMu;
static std::vector<uint32_t> g_callRoots;
static constexpr size_t kMaxCallRoots=16384;

static void retainCallResult(Il2CppObject*o){
 if(!o||!Il2cpp::GcHandleApiResolved())return;
 std::lock_guard<std::mutex>lk(g_rootMu);
 if(g_callRoots.size()>=kMaxCallRoots)return;
 uint32_t h=Il2cpp::GC::NewHandle(o,false);
 if(h)g_callRoots.push_back(h);
}

static void logf(const char *fmt,...){
 char b[1024]; va_list ap; va_start(ap,fmt); vsnprintf(b,sizeof(b),fmt,ap); va_end(ap);
 __android_log_print(ANDROID_LOG_INFO,"AZL07","%s",b);
 FILE *f=fopen("/data/local/tmp/AZTool/core.log","a"); if(f){fprintf(f,"[AZL07] %s\n",b);fclose(f);}
}
static void ensureDir(){
 g_pkg=procPackage();
 std::string files="/data/user/0/"+g_pkg+"/files";
 g_base=files+"/AZTool"; g_req=g_base+"/script.req"; g_out=g_base+"/script.out"; g_status=g_base+"/script.status";
 mkdir(files.c_str(),0700); mkdir(g_base.c_str(),0700);
}
static void writeText(const char *p,const std::string&s){FILE*f=fopen(p,"wb");if(!f)return;fwrite(s.data(),1,s.size(),f);fclose(f);}
static std::string readText(const char*p,size_t lim=1048576){FILE*f=fopen(p,"rb");if(!f)return{};std::string s;char b[4096];while(!feof(f)&&s.size()<lim){size_t n=fread(b,1,sizeof(b),f);if(!n)break;if(s.size()+n>lim)n=lim-s.size();s.append(b,n);}fclose(f);return s;}
static long long monoMs(){timespec t{};clock_gettime(CLOCK_MONOTONIC,&t);return (long long)t.tv_sec*1000LL+t.tv_nsec/1000000LL;}
static void writeStatus(const char*state,const std::string&name="",long long startMs=0,long long elapsedMs=0,const std::string&msg=""){
 char b[1024];
 snprintf(b,sizeof(b),"state=%s\nname=%s\nstart_ms=%lld\nelapsed_ms=%lld\nmessage=%s\n",
          state?state:"UNKNOWN",name.c_str(),startMs,elapsedMs,msg.c_str());
 std::string tmp=g_status+".tmp";
 writeText(tmp.c_str(),b);
 rename(tmp.c_str(),g_status.c_str());
}

static void luaStopHook(lua_State*L,lua_Debug*){
 if(g_stopRequested.load(std::memory_order_relaxed))
  luaL_error(L,"__AZ_STOP_REQUESTED__");
}

static void requestStop(){
 g_stopRequested.store(true,std::memory_order_relaxed);
 std::string name; long long start=0;
 {
  std::lock_guard<std::mutex>lk(g_runMetaMu);
  name=g_runningName; start=g_runningStartMs;
 }
 long long elapsed=start>0?std::max(0LL,monoMs()-start):0;
 writeStatus("STOPPING",name,start,elapsed,"stop requested");
 logf("Stop requested | name=%s elapsed=%lldms",name.c_str(),elapsed);
}
static void out(const std::string&s){FILE*f=fopen(OUT(),"ab");if(!f)return;fwrite(s.data(),1,s.size(),f);fwrite("\n",1,1,f);fclose(f);}
static void trace(const std::string&s){out("[TRACE] "+s);logf("TRACE %s",s.c_str());}

static std::string tname(Il2CppType*t){const char*n=t?Il2cpp::GetTypeName(t):nullptr;return n?n:"";}
static std::string shortn(std::string s){if(s.rfind("System.",0)==0)s.erase(0,7);return s;}
static bool teq(std::string a,std::string b){return a==b||shortn(a)==shortn(b);}
static Obj* ckobj(lua_State*L,int i){return(Obj*)luaL_checkudata(L,i,"AZ.Obj");}
static Cls* ckcls(lua_State*L,int i){return(Cls*)luaL_checkudata(L,i,"AZ.Cls");}
static Il2CppObject* resolve(Obj*u){
 // Unity 6000.4 + Houdini: il2cpp_gchandle_get_target can crash in libil2cpp.
 // Keep GCHandle only as a strong root. IL2CPP objects are non-moving here,
 // so the rooted raw pointer remains stable for the wrapper lifetime.
 return u?u->raw:nullptr;
}
static int pushObj(lua_State*L,Il2CppObject*o){
 if(!o){lua_pushnil(L);return 1;}
 auto*u=(Obj*)lua_newuserdatauv(L,sizeof(Obj),0);
 new(u)Obj{};
 u->raw=o;
 u->handle=0; // SilverV7 findObjects userdata: raw pointer + zero owned handle.
 luaL_getmetatable(L,"AZ.Obj");
 lua_setmetatable(L,-2);
 return 1;
}
static int pushCls(lua_State*L,Il2CppClass*k){if(!k){lua_pushnil(L);return 1;}auto*u=(Cls*)lua_newuserdatauv(L,sizeof(Cls),0);u->klass=k;luaL_getmetatable(L,"AZ.Cls");lua_setmetatable(L,-2);return 1;}
static int objgc(lua_State*L){
 auto*u=(Obj*)luaL_testudata(L,1,"AZ.Obj");
 if(u){u->raw=nullptr;u->handle=0;}
 return 0;
}
static int objstr(lua_State*L){auto*o=resolve(ckobj(L,1));if(!o){lua_pushliteral(L,"<managed:null>");return 1;}auto*k=Il2cpp::GetObjectClass(o);lua_pushfstring(L,"<managed:%s@%p>",k?k->getName():"?",o);return 1;}
static int clsstr(lua_State*L){auto*c=ckcls(L,1);lua_pushfstring(L,"<class:%s>",c&&c->klass?c->klass->getFullName().c_str():"null");return 1;}
static bool isEnum(Il2CppType*t){auto*k=t?t->getClass():nullptr;return k&&Il2cpp::GetClassIsEnum(k);}

static int pushManaged(lua_State*L,Il2CppObject*o,Il2CppType*t){
 std::string n=tname(t);
 if(n=="System.Void"||n=="Void"){lua_pushnil(L);return 1;}
 if(!o){lua_pushnil(L);return 1;}
 if(n=="System.String"||n=="String"){
  lua_pushstring(L,((Il2CppString*)o)->to_string().c_str());
  return 1;
 }

 // IMPORTANT: il2cpp_object_unbox is only valid for boxed value types.
 // SilverV7 keeps normal managed references (List<T>, classes, arrays, etc.)
 // as object handles.  The old AZ bridge unboxed every return value first,
 // which can SIGSEGV as soon as a method returns a List<T>.
 auto unboxValue=[&]()->void*{return Il2cpp::GetUnboxedValue(o);};

 if(n=="System.Boolean"||n=="Boolean"){void*p=unboxValue();lua_pushboolean(L,p?*(bool*)p:false);return 1;}
 if(n=="System.SByte"){void*p=unboxValue();lua_pushinteger(L,p?*(int8_t*)p:0);return 1;}
 if(n=="System.Byte"){void*p=unboxValue();lua_pushinteger(L,p?*(uint8_t*)p:0);return 1;}
 if(n=="System.Int16"){void*p=unboxValue();lua_pushinteger(L,p?*(int16_t*)p:0);return 1;}
 if(n=="System.UInt16"||n=="System.Char"){void*p=unboxValue();lua_pushinteger(L,p?*(uint16_t*)p:0);return 1;}
 if(n=="System.Int32"||n=="Int32"){void*p=unboxValue();lua_pushinteger(L,p?*(int32_t*)p:0);return 1;}
 if(n=="System.UInt32"||n=="UInt32"){void*p=unboxValue();lua_pushinteger(L,p?*(uint32_t*)p:0);return 1;}
 if(n=="System.Int64"||n=="Int64"){void*p=unboxValue();lua_pushinteger(L,p?(lua_Integer)*(int64_t*)p:0);return 1;}
 if(n=="System.UInt64"||n=="UInt64"){void*p=unboxValue();lua_pushinteger(L,p?(lua_Integer)*(uint64_t*)p:0);return 1;}
 if(n=="System.Single"||n=="Single"){void*p=unboxValue();lua_pushnumber(L,p?*(float*)p:0);return 1;}
 if(n=="System.Double"||n=="Double"){void*p=unboxValue();lua_pushnumber(L,p?*(double*)p:0);return 1;}
 if(isEnum(t)){
  void*p=unboxValue();auto*b=Il2cpp::GetEnumBaseType(t->getClass());std::string bn=tname(b);
  if(bn=="System.Int64")lua_pushinteger(L,p?(lua_Integer)*(int64_t*)p:0);
  else if(bn=="System.UInt64")lua_pushinteger(L,p?(lua_Integer)*(uint64_t*)p:0);
  else if(bn=="System.Int16")lua_pushinteger(L,p?*(int16_t*)p:0);
  else if(bn=="System.UInt16")lua_pushinteger(L,p?*(uint16_t*)p:0);
  else if(bn=="System.Byte")lua_pushinteger(L,p?*(uint8_t*)p:0);
  else if(bn=="System.SByte")lua_pushinteger(L,p?*(int8_t*)p:0);
  else if(bn=="System.UInt32")lua_pushinteger(L,p?*(uint32_t*)p:0);
  else lua_pushinteger(L,p?*(int32_t*)p:0);
  return 1;
 }

 // Reference/array/list/class: SilverV7 does not make ordinary Lua
 // userdata own a GCHandle. Keep an external root for Call results instead,
 // so Lua collectgarbage() cannot call il2cpp_gchandle_free on the wrapper.
 retainCallResult(o);
 return pushObj(L,o);
}
static Il2CppObject* box(lua_State*L,int i,Il2CppType*t){
 if(lua_isnil(L,i))return nullptr;if(luaL_testudata(L,i,"AZ.Obj"))return resolve(ckobj(L,i));
 std::string n=tname(t);auto*k=t?t->getClass():nullptr;if(n=="System.String"||n=="String")return(Il2CppObject*)Il2cpp::NewString(luaL_checkstring(L,i));if(!k)return nullptr;
 if(n=="System.Boolean"||n=="Boolean"){bool v=lua_toboolean(L,i);return Il2cpp::GetBoxedValue(k,&v);}
 if(n=="System.SByte"){int8_t v=(int8_t)luaL_checkinteger(L,i);return Il2cpp::GetBoxedValue(k,&v);} if(n=="System.Byte"){uint8_t v=(uint8_t)luaL_checkinteger(L,i);return Il2cpp::GetBoxedValue(k,&v);}
 if(n=="System.Int16"){int16_t v=(int16_t)luaL_checkinteger(L,i);return Il2cpp::GetBoxedValue(k,&v);} if(n=="System.UInt16"||n=="System.Char"){uint16_t v=(uint16_t)luaL_checkinteger(L,i);return Il2cpp::GetBoxedValue(k,&v);}
 if(n=="System.Int32"||n=="Int32"){int32_t v=(int32_t)luaL_checkinteger(L,i);return Il2cpp::GetBoxedValue(k,&v);} if(n=="System.UInt32"||n=="UInt32"){uint32_t v=(uint32_t)luaL_checkinteger(L,i);return Il2cpp::GetBoxedValue(k,&v);}
 if(n=="System.Int64"||n=="Int64"){int64_t v=(int64_t)luaL_checkinteger(L,i);return Il2cpp::GetBoxedValue(k,&v);} if(n=="System.UInt64"||n=="UInt64"){uint64_t v=(uint64_t)luaL_checkinteger(L,i);return Il2cpp::GetBoxedValue(k,&v);}
 if(n=="System.Single"||n=="Single"){float v=(float)luaL_checknumber(L,i);return Il2cpp::GetBoxedValue(k,&v);} if(n=="System.Double"||n=="Double"){double v=(double)luaL_checknumber(L,i);return Il2cpp::GetBoxedValue(k,&v);}
 if(isEnum(t)){auto*b=Il2cpp::GetEnumBaseType(k);std::string bn=tname(b);if(bn=="System.Int64"){int64_t v=luaL_checkinteger(L,i);return Il2cpp::GetBoxedValue(k,&v);}if(bn=="System.UInt64"){uint64_t v=(uint64_t)luaL_checkinteger(L,i);return Il2cpp::GetBoxedValue(k,&v);}int32_t v=(int32_t)luaL_checkinteger(L,i);return Il2cpp::GetBoxedValue(k,&v);}
 return nullptr;
}
static MethodInfo* findMethod(Il2CppClass*k,const std::string&name,int ac,const std::vector<std::string>*want=nullptr){
 for(;k;k=Il2cpp::GetClassParent(k)){void*it=nullptr;while(auto*m=Il2cpp::GetClassMethods(k,&it)){const char*mn=Il2cpp::GetMethodName(m);if(!mn||name!=mn)continue;int n=(int)Il2cpp::GetMethodParamCount(m);if(n!=ac)continue;if(want&&(int)want->size()==n){bool ok=true;for(int j=0;j<n;j++)if(!teq(tname(Il2cpp::GetMethodParam(m,j)),(*want)[j])){ok=false;break;}if(!ok)continue;}return m;}}return nullptr;
}

/* ---------- V7-style Unity main-thread dispatcher ----------
   The current SilverV7 payload bootstraps from UnitySynchronizationContext,
   keeps the context + SendOrPostCallback alive with GCHandles, and wakes
   UnityMain through SynchronizationContext.Post.  We mirror that model here.
*/
struct AZDelegateShim {
 Il2CppObject object;
 void *method_ptr;
 void *invoke_impl;
 Il2CppObject *target;
 MethodInfo *method;
 void *delegate_trampoline;
 intptr_t extra_arg;
};

struct MainInvokeTask {
 MethodInfo *method{};
 Il2CppObject *recv{};
 std::vector<Il2CppObject*> args;
 Il2CppObject *result{};
 Il2CppException *exception{};
 uint32_t resultHandle{};
 bool done{};
 std::mutex mu;
 std::condition_variable cv;
};

struct MainScriptTask {
 std::string name;
 bool done{};
 std::mutex mu;
 std::condition_variable cv;
};

using SyncExecFn = void(*)(Il2CppObject*,MethodInfo*);
static SyncExecFn g_syncExecOrig=nullptr;
static void *g_syncExecTarget=nullptr;
static std::atomic<bool> g_mainHookInstalled{false};
static std::atomic<bool> g_mainReady{false};
static std::atomic<int> g_unityMainTid{0};
static std::mutex g_mainSetupMu;
static std::condition_variable g_mainReadyCv;
static std::mutex g_mainQueueMu;
static std::deque<MainInvokeTask*> g_mainQueue;
static std::mutex g_scriptQueueMu;
static std::deque<MainScriptTask*> g_scriptQueue;
static void run(const std::string&name);
static Il2CppObject *g_syncContextRaw=nullptr;
static Il2CppObject *g_sendDelegateRaw=nullptr;
static uint32_t g_syncContextHandle=0;
static uint32_t g_sendDelegateHandle=0;
static MethodInfo *g_syncPostMethod=nullptr;

static Il2CppObject* rooted(uint32_t h,Il2CppObject*raw){
 (void)h;
 return raw;
}

static void finishMainTask(MainInvokeTask*t){
 if(!t)return;
 {
  std::lock_guard<std::mutex>lk(t->mu);
  t->done=true;
 }
 t->cv.notify_one();
}

/* SendOrPostCallback native target.
   SilverV7's thunk ignores the managed state argument as well and drains
   its native queue on UnityMain. */
static void mainPostThunk(){
 g_unityMainTid.store((int)syscall(SYS_gettid));
 std::deque<MainInvokeTask*> q;
 {
  std::lock_guard<std::mutex>lk(g_mainQueueMu);
  q.swap(g_mainQueue);
 }
 for(auto*t:q){
  if(!t)continue;
  Il2CppException*ex=nullptr;
  Il2CppObject*r=nullptr;
  try{
   if(t->args.empty()) r=Il2cpp::RuntimeInvoke(t->method,t->recv,nullptr,&ex);
   else r=Il2cpp::RuntimeInvokeConvertArgs(
      t->method,t->recv,t->args.data(),(int)t->args.size(),&ex);
  }catch(...){
   ex=(Il2CppException*)1;
  }
  t->result=r;
  t->exception=ex;
  if(r&&Il2cpp::GcHandleApiResolved())t->resultHandle=Il2cpp::GC::NewHandle(r,false);
  finishMainTask(t);
 }
}

static bool setupMainDispatcher(Il2CppObject*ctx){
 if(!ctx)return false;
 std::lock_guard<std::mutex>lk(g_mainSetupMu);
 if(g_mainReady.load())return true;

 auto*ctxClass=Il2cpp::GetObjectClass(ctx);
 if(!ctxClass)return false;
 auto*post=findMethod(ctxClass,"Post",2);
 if(!post){
  logf("UnityMain bootstrap: SynchronizationContext.Post not found");
  return false;
 }

 auto*delegateClass=Il2cpp::FindClass("System.Threading.SendOrPostCallback");
 if(!delegateClass){
  logf("UnityMain bootstrap: SendOrPostCallback class not found");
  return false;
 }
 auto*del=delegateClass->New();
 if(!del){
  logf("UnityMain bootstrap: failed to allocate SendOrPostCallback");
  return false;
 }

 auto*shim=reinterpret_cast<AZDelegateShim*>(del);
 shim->method_ptr=(void*)mainPostThunk;
 shim->invoke_impl=(void*)mainPostThunk;
 shim->target=nullptr;
 shim->method=nullptr;
 shim->delegate_trampoline=nullptr;
 shim->extra_arg=0;

 g_syncContextRaw=ctx;
 g_sendDelegateRaw=del;
 g_syncPostMethod=post;
 if(Il2cpp::GcHandleApiResolved()){
  g_syncContextHandle=Il2cpp::GC::NewHandle(ctx,false);
  g_sendDelegateHandle=Il2cpp::GC::NewHandle(del,false);
 }
 g_unityMainTid.store((int)syscall(SYS_gettid));
 g_mainReady.store(true);
 g_mainReadyCv.notify_all();
 logf("UnityMain dispatcher READY tid=%d context=%p delegate=%p Post=%p",
      g_unityMainTid.load(),ctx,del,post);
 return true;
}

static void drainScriptQueueOnUnityMain(){
 MainScriptTask*t=nullptr;
 {
  std::lock_guard<std::mutex>lk(g_scriptQueueMu);
  if(!g_scriptQueue.empty()){t=g_scriptQueue.front();g_scriptQueue.pop_front();}
 }
 if(!t)return;
 g_unityMainTid.store((int)syscall(SYS_gettid));
 logf("UnityMain script begin name=%s tid=%d",t->name.c_str(),g_unityMainTid.load());
 run(t->name);
 logf("UnityMain script end name=%s",t->name.c_str());
 {
  std::lock_guard<std::mutex>lk(t->mu);t->done=true;
 }
 t->cv.notify_one();
}

static void syncExecHook(Il2CppObject*self,MethodInfo*m){
 if(self&&!g_mainReady.load())setupMainDispatcher(self);
 auto orig=g_syncExecOrig;
 if(orig)orig(self,m);
 // SilverV7 executes the Lua body synchronously on UnityMain.
 // Drain at most one queued script per UnitySynchronizationContext::Exec frame.
 drainScriptQueueOnUnityMain();
}

static bool installMainThreadBootstrap(){
 if(g_mainHookInstalled.load())return true;
 auto*c=Il2cpp::FindClass("UnityEngine.UnitySynchronizationContext");
 if(!c){
  logf("UnityMain bootstrap: class unavailable");
  return false;
 }
 MethodInfo*m=findMethod(c,"Exec",0);
 if(!m)m=findMethod(c,"ExecuteTasks",0);
 if(!m||!m->methodPointer){
  logf("UnityMain bootstrap: Exec/ExecuteTasks unavailable");
  return false;
 }
 void*orig=nullptr;
 int rc=DobbyHook(m->methodPointer,(void*)syncExecHook,&orig);
 if(rc!=0||!orig){
  logf("UnityMain bootstrap: DobbyHook failed rc=%d",rc);
  return false;
 }
 g_syncExecTarget=m->methodPointer;
 g_syncExecOrig=(SyncExecFn)orig;
 g_mainHookInstalled.store(true);
 logf("UnityMain bootstrap hook installed method=%s target=%p orig=%p",
      Il2cpp::GetMethodName(m),g_syncExecTarget,orig);
 return true;
}

static bool waitMainDispatcher(int timeoutMs){
 if(g_mainReady.load())return true;
 installMainThreadBootstrap();
 if(g_mainReady.load())return true;
 std::unique_lock<std::mutex>lk(g_mainSetupMu);
 return g_mainReadyCv.wait_for(lk,std::chrono::milliseconds(timeoutMs),[]{
  return g_mainReady.load();
 });
}

static bool postMainWake(){
 auto*ctx=rooted(g_syncContextHandle,g_syncContextRaw);
 auto*del=rooted(g_sendDelegateHandle,g_sendDelegateRaw);
 auto*post=g_syncPostMethod;
 if(!ctx||!del||!post)return false;

 /* Match V7's Post call shape: managed ref parameters passed through
    il2cpp_runtime_invoke as addresses of object references. */
 Il2CppObject*arg0=del;
 Il2CppObject*arg1=nullptr;
 void*params[2]={&arg0,&arg1};
 Il2CppException*ex=nullptr;
 try{
  Il2cpp::RuntimeInvoke(post,ctx,params,&ex);
 }catch(...){
  ex=(Il2CppException*)1;
 }
 if(ex){
  logf("UnityMain Post threw");
  return false;
 }
 return true;
}

struct MainInvokeReply{
 Il2CppObject*result{};
 uint32_t resultHandle{};
 bool exception{};
 bool timeout{};
 bool unavailable{};
};

static MainInvokeReply invokeOnUnityMain(MethodInfo*m,Il2CppObject*recv,
                                         const std::vector<Il2CppObject*>&args){
 MainInvokeReply rep{};
 if(!m){rep.unavailable=true;return rep;}

 int tid=(int)syscall(SYS_gettid);
 if(g_mainReady.load()&&tid==g_unityMainTid.load()){
  Il2CppException*ex=nullptr;
  try{
   if(args.empty()) rep.result=Il2cpp::RuntimeInvoke(m,recv,nullptr,&ex);
   else rep.result=Il2cpp::RuntimeInvokeConvertArgs(
     m,recv,const_cast<Il2CppObject**>(args.data()),(int)args.size(),&ex);
  }catch(...){ex=(Il2CppException*)1;}
  rep.exception=(ex!=nullptr);
  return rep;
 }

 if(!waitMainDispatcher(4000)){
  rep.unavailable=true;
  logf("UnityMain dispatcher unavailable for %s",Il2cpp::GetMethodName(m));
  return rep;
 }

 /* Keep receiver + boxed arguments alive while waiting for UnityMain. */
 uint32_t recvH=0;
 std::vector<uint32_t> argH(args.size(),0);
 if(Il2cpp::GcHandleApiResolved()){
  if(recv)recvH=Il2cpp::GC::NewHandle(recv,false);
  for(size_t i=0;i<args.size();++i)if(args[i])argH[i]=Il2cpp::GC::NewHandle(args[i],false);
 }

 MainInvokeTask task;
 task.method=m;
 task.recv=rooted(recvH,recv);
 task.args.resize(args.size());
 for(size_t i=0;i<args.size();++i)task.args[i]=rooted(argH[i],args[i]);

 {
  std::lock_guard<std::mutex>lk(g_mainQueueMu);
  g_mainQueue.push_back(&task);
 }

 logf("UnityMain queue %s recv=%p argc=%zu",Il2cpp::GetMethodName(m),task.recv,args.size());
 if(!postMainWake()){
  {
   std::lock_guard<std::mutex>lk(g_mainQueueMu);
   auto it=std::find(g_mainQueue.begin(),g_mainQueue.end(),&task);
   if(it!=g_mainQueue.end())g_mainQueue.erase(it);
  }
  rep.unavailable=true;
 }else{
  std::unique_lock<std::mutex>lk(task.mu);
  if(!task.cv.wait_for(lk,std::chrono::milliseconds(5000),[&]{return task.done;})){
   rep.timeout=true;
   logf("UnityMain invoke timeout method=%s",Il2cpp::GetMethodName(m));
  }else{
   rep.result=rooted(task.resultHandle,task.result);
   rep.resultHandle=task.resultHandle;
   rep.exception=(task.exception!=nullptr);
   logf("UnityMain invoke done method=%s result=%p exception=%d",
        Il2cpp::GetMethodName(m),rep.result,rep.exception?1:0);
  }
 }

 if(recvH)Il2cpp::GC::FreeHandle(recvH);
 for(auto h:argH)if(h)Il2cpp::GC::FreeHandle(h);
 return rep;
}

static void releaseReply(MainInvokeReply&r){
 if(r.resultHandle){Il2cpp::GC::FreeHandle(r.resultHandle);r.resultHandle=0;}
}

static bool desc(lua_State*L,int i,std::string&d,std::string&n,std::vector<std::string>&p){
 if(!lua_istable(L,i))return false;lua_getfield(L,i,"declaring");if(const char*s=lua_tostring(L,-1))d=s;lua_pop(L,1);lua_getfield(L,i,"name");if(const char*s=lua_tostring(L,-1))n=s;lua_pop(L,1);lua_getfield(L,i,"params");if(lua_istable(L,-1)){lua_Integer z=lua_rawlen(L,-1);for(lua_Integer x=1;x<=z;x++){lua_geti(L,-1,x);const char*s=lua_tostring(L,-1);p.emplace_back(s?s:"");lua_pop(L,1);}}lua_pop(L,1);return!d.empty()&&!n.empty();
}
static MethodInfo* fromDesc(lua_State*L,int i){std::string d,n;std::vector<std::string>p;if(!desc(L,i,d,n,p))return nullptr;auto*k=Il2cpp::FindClass(d.c_str());return k?findMethod(k,n,(int)p.size(),&p):nullptr;}
static int invoke(lua_State*L,MethodInfo*m,Il2CppObject*recv,int first,int ac){
 if(!m)return luaL_error(L,"method not found");
 const char*mn=Il2cpp::GetMethodName(m);if(!mn)mn="?";
 trace(std::string("invoke.begin | ")+mn+" | recv="+std::to_string((uintptr_t)recv)+" | argc="+std::to_string(ac));

 int n=(int)Il2cpp::GetMethodParamCount(m);
 trace(std::string("invoke.param_count | ")+mn+" | need="+std::to_string(n));
 if(n!=ac)return luaL_error(L,"arg count need=%d got=%d",n,ac);

 std::vector<Il2CppObject*>a(n);
 for(int j=0;j<n;j++){
  auto*t=Il2cpp::GetMethodParam(m,j);
  trace(std::string("invoke.marshal.begin | ")+mn+" | arg="+std::to_string(j+1)+" | type="+tname(t));
  a[j]=box(L,first+j,t);
  if(!lua_isnil(L,first+j)&&!a[j]&&!luaL_testudata(L,first+j,"AZ.Obj"))
   return luaL_error(L,"marshal arg %d as %s failed",j+1,tname(t).c_str());
  trace(std::string("invoke.marshal.done | ")+mn+" | arg="+std::to_string(j+1));
 }

 trace(std::string("invoke.runtime.begin | ")+mn+(n==0?" | RuntimeInvoke":" | ConvertArgs"));
 auto rep=invokeOnUnityMain(m,recv,a);
 trace(std::string("invoke.runtime.done | ")+mn+" | result="+std::to_string((uintptr_t)rep.result)+" | ex="+(rep.exception?"1":"0"));

 if(rep.unavailable)return luaL_error(L,"Unity main-thread dispatcher unavailable in %s",mn);
 if(rep.timeout)return luaL_error(L,"Unity main-thread invoke timeout in %s",mn);
 if(rep.exception){releaseReply(rep);return luaL_error(L,"managed exception in %s",mn);}

 auto*rt=Il2cpp::GetMethodReturnType(m);
 trace(std::string("invoke.return.begin | ")+mn+" | type="+tname(rt));
 int rc=pushManaged(L,rep.result,rt);
 trace(std::string("invoke.return.done | ")+mn);
 releaseReply(rep);
 return rc;
}

static int classFrom(lua_State*L){return pushCls(L,Il2cpp::FindClass(luaL_checkstring(L,1)));}

static bool classIsOrDerived(Il2CppClass* actual,Il2CppClass* wanted){
 if(!actual||!wanted)return false;
 for(auto*k=actual;k;k=Il2cpp::GetClassParent(k))if(k==wanted)return true;
 return false;
}

static void dedupeObjects(std::vector<Il2CppObject*>&v){
 std::sort(v.begin(),v.end());
 v.erase(std::unique(v.begin(),v.end()),v.end());
}

static void collectSingletonGetter(Il2CppClass*k,std::vector<Il2CppObject*>&out){
 static const char* names[]={"get_Ins","get_Instance","get_instance","get_Singleton","get_Current",nullptr};
 for(int i=0;names[i];++i){
  auto*m=findMethod(k,names[i],0);
  if(!m||!Il2cpp::GetIsMethodStatic(m))continue;
  Il2CppException*e=nullptr;
  auto*o=Il2cpp::RuntimeInvokeConvertArgs(m,nullptr,nullptr,0,&e);
  if(e||!o)continue;
  auto*oc=Il2cpp::GetObjectClass(o);
  if(classIsOrDerived(oc,k))out.push_back(o);
 }
}

static void collectUnityResources(Il2CppClass*k,std::vector<Il2CppObject*>&out){
 if(!k)return;
 auto*res=Il2cpp::FindClass("UnityEngine.Resources");
 auto*typeObj=Il2cpp::GetTypeObject(Il2cpp::GetClassType(k));
 if(!res||!typeObj)return;
 std::vector<std::string>want={"System.Type"};
 auto*m=findMethod(res,"FindObjectsOfTypeAll",1,&want);
 if(!m||!Il2cpp::GetIsMethodStatic(m))return;
 Il2CppObject*args[1]={typeObj};
 Il2CppException*e=nullptr;
 auto*arr=Il2cpp::RuntimeInvokeConvertArgs(m,nullptr,args,1,&e);
 if(e||!arr)return;

 uint32_t n=Il2cpp::GetArrayLength((_Il2CppArray*)arr);
 if(n>100000)n=100000;
 auto*intK=Il2cpp::FindClass("System.Int32");
 if(!intK)return;
 std::vector<std::string>iwant={"System.Int32"};
 auto*getValue=findMethod(Il2cpp::GetObjectClass(arr),"GetValue",1,&iwant);
 if(!getValue)return;
 out.reserve(out.size()+n);
 for(uint32_t i=0;i<n;i++){
  int32_t idx=(int32_t)i;
  auto*ib=Il2cpp::GetBoxedValue(intK,&idx);
  if(!ib)continue;
  Il2CppObject*ga[1]={ib};
  Il2CppException*ge=nullptr;
  auto*o=Il2cpp::RuntimeInvokeConvertArgs(getValue,arr,ga,1,&ge);
  if(ge||!o)continue;
  auto*oc=Il2cpp::GetObjectClass(o);
  if(classIsOrDerived(oc,k))out.push_back(o);
 }
}

static std::vector<Il2CppObject*> findObjectsSafe(Il2CppClass*k){
 std::vector<Il2CppObject*>v;
 if(!k)return v;
 collectSingletonGetter(k,v);
 collectUnityResources(k,v);
 dedupeObjects(v);
 logf("findObjectsFresh safe class=%s count=%zu",k->getFullName().c_str(),v.size());
 return v;
}

static int pushObjectList(lua_State*L,std::vector<Il2CppObject*>v){
 lua_createtable(L,(int)v.size(),0);int i=1;
 for(auto*o:v){if(!o)continue;pushObj(L,o);lua_seti(L,-2,i++);}
 return 1;
}

static int findObjectsFresh(lua_State*L){
 auto*c=ckcls(L,1);
 std::string cn=(c&&c->klass)?c->klass->getFullName():"?";
 trace("findObjectsFresh.begin | "+cn);
 auto v=(c&&c->klass)?findObjectsSafe(c->klass):std::vector<Il2CppObject*>{};
 trace("findObjectsFresh.done | "+cn+" | count="+std::to_string(v.size()));
 return pushObjectList(L,std::move(v));
}

static int findObjectsHeap(lua_State*L){
 auto*c=ckcls(L,1);
 auto v=(c&&c->klass)?Il2cpp::GC::FindObjects(c->klass):std::vector<Il2CppObject*>{};
 logf("findObjectsHeap liveness class=%s count=%zu",(c&&c->klass)?c->klass->getFullName().c_str():"?",v.size());
 return pushObjectList(L,std::move(v));
}

static int clsIndex(lua_State*L){
 auto*c=ckcls(L,1);const char*k=luaL_checkstring(L,2);
 if(!strcmp(k,"findObjectsFresh")||!strcmp(k,"findObjects")){lua_pushcfunction(L,findObjectsFresh);return 1;}
 if(!strcmp(k,"findObjectsHeap")){lua_pushcfunction(L,findObjectsHeap);return 1;}
 if(!strcmp(k,"name")){lua_pushstring(L,c&&c->klass?c->klass->getFullName().c_str():"");return 1;}
 lua_pushnil(L);return 1;
}
static int getField(lua_State*L){auto*o=resolve(ckobj(L,1));const char*n=luaL_checkstring(L,2);if(!o){lua_pushnil(L);return 1;}auto*k=Il2cpp::GetObjectClass(o);auto*f=k?k->getFieldInHierarchy(n):nullptr;if(!f){lua_pushnil(L);return 1;}return pushManaged(L,Il2cpp::GetFieldValueObject(o,f),f->getType());}
static int setField(lua_State*L){auto*o=resolve(ckobj(L,1));const char*n=luaL_checkstring(L,2);if(!o)return luaL_error(L,"null object");auto*k=Il2cpp::GetObjectClass(o);auto*f=k?k->getFieldInHierarchy(n):nullptr;if(!f)return luaL_error(L,"field not found: %s",n);auto*b=box(L,3,f->getType());if(f->getType()->isObject()||f->getType()->isArray())Il2cpp::SetFieldValueObject(o,f,b);else{void*p=b?Il2cpp::GetUnboxedValue(b):nullptr;if(!p)return luaL_error(L,"field marshal failed");Il2cpp::SetFieldValue(o,f,p);}lua_pushboolean(L,1);return 1;}
static int dynCall(lua_State*L){const char*n=lua_tostring(L,lua_upvalueindex(1));auto*o=resolve(ckobj(L,1));if(!o)return luaL_error(L,"null object");int ac=lua_gettop(L)-1;auto*m=findMethod(Il2cpp::GetObjectClass(o),n?n:"",ac);if(!m)return luaL_error(L,"method not found: %s",n?n:"?");return invoke(L,m,o,2,ac);}
static int objIndex(lua_State*L){auto*o=resolve(ckobj(L,1));const char*k=luaL_checkstring(L,2);if(!strcmp(k,"getField")){lua_pushcfunction(L,getField);return 1;}if(!strcmp(k,"setField")){lua_pushcfunction(L,setField);return 1;}if(!o){lua_pushnil(L);return 1;}auto*c=Il2cpp::GetObjectClass(o);if(auto*f=c?c->getFieldInHierarchy(k):nullptr)return pushManaged(L,Il2cpp::GetFieldValueObject(o,f),f->getType());if(strncmp(k,"get_",4)&&strncmp(k,"set_",4)){std::string g="get_";g+=k;if(auto*m=findMethod(c,g,0))return invoke(L,m,o,0,0);}lua_pushstring(L,k);lua_pushcclosure(L,dynCall,1);return 1;}
static int callExact(lua_State*L){
 std::string d,n;std::vector<std::string>p;
 if(!desc(L,1,d,n,p))return luaL_error(L,"Call.exact invalid descriptor");
 trace("Call.exact.begin | "+d+"::"+n+" | desc_params="+std::to_string(p.size()));
 auto*k=Il2cpp::FindClass(d.c_str());
 trace("Call.exact.class | "+d+" | ptr="+std::to_string((uintptr_t)k));
 if(!k)return luaL_error(L,"Call.exact class not found: %s",d.c_str());
 auto*m=findMethod(k,n,(int)p.size(),&p);
 trace("Call.exact.method | "+n+" | ptr="+std::to_string((uintptr_t)m));
 if(!m)return luaL_error(L,"Call.exact method not found");
 Il2CppObject*r=nullptr;
 if(auto*u=(Obj*)luaL_testudata(L,2,"AZ.Obj")){
  trace("Call.exact.receiver.wrapper | "+n+" | raw="+std::to_string((uintptr_t)u->raw)+" | handle="+std::to_string(u->handle));
  r=resolve(u);
 }
 trace("Call.exact.receiver | "+n+" | ptr="+std::to_string((uintptr_t)r));
 return invoke(L,m,r,3,lua_gettop(L)-2);
}
static int arrayGet(lua_State*L){auto*o=resolve(ckobj(L,1));auto*m=o?findMethod(Il2cpp::GetObjectClass(o),"GetValue",1):nullptr;if(!m)return luaL_error(L,"Array.GetValue unavailable");return invoke(L,m,o,2,1);}
static int arrayLen(lua_State*L){auto*o=resolve(ckobj(L,1));lua_pushinteger(L,o?(lua_Integer)Il2cpp::GetArrayLength((_Il2CppArray*)o):0);return 1;}

static bool retTrue(){return true;} static bool retFalse(){return false;}
static int install(lua_State*L,MethodInfo*m,void*rep){if(!m||!m->methodPointer||!rep)return luaL_error(L,"invalid hook");void*orig=nullptr;int rc=DobbyHook(m->methodPointer,rep,&orig);if(rc||!orig)return luaL_error(L,"DobbyHook rc=%d",rc);int id=nextHook.fetch_add(1);std::lock_guard<std::mutex>g(hookMu);hooks[id]={id,m,m->methodPointer,orig};lua_pushinteger(L,id);return 1;}
static int unbridge(lua_State*L){int id=luaL_checkinteger(L,1);std::lock_guard<std::mutex>g(hookMu);auto it=hooks.find(id);if(it==hooks.end()){lua_pushboolean(L,0);return 1;}int rc=DobbyDestroy(it->second.target);hooks.erase(it);lua_pushboolean(L,rc==0);return 1;}
static int bridge(lua_State*L){luaL_checktype(L,1,LUA_TTABLE);lua_getfield(L,1,"source");auto*s=fromDesc(L,-1);lua_pop(L,1);if(!s)return luaL_error(L,"Hook.bridge source not found");std::string mode;lua_getfield(L,1,"mode");if(const char*x=lua_tostring(L,-1))mode=x;lua_pop(L,1);lua_getfield(L,1,"target");auto*t=fromDesc(L,-1);lua_pop(L,1);if(mode=="trigger_redirect"){bool v=false,has=false;lua_getfield(L,1,"args");if(lua_istable(L,-1)&&lua_rawlen(L,-1)){lua_geti(L,-1,1);if(const char*a=lua_tostring(L,-1)){std::string q=a;if(q.find("true")!=std::string::npos){v=true;has=true;}else if(q.find("false")!=std::string::npos){v=false;has=true;}}lua_pop(L,1);}lua_pop(L,1);std::string rt=tname(Il2cpp::GetMethodReturnType(s));if(has&&(rt=="System.Boolean"||rt=="Boolean"))return install(L,s,v?(void*)retTrue:(void*)retFalse);}if(t&&Il2cpp::GetMethodParamCount(t)==Il2cpp::GetMethodParamCount(s)&&teq(tname(Il2cpp::GetMethodReturnType(t)),tname(Il2cpp::GetMethodReturnType(s))))return install(L,s,t->methodPointer);return luaL_error(L,"Hook.bridge unsupported signature");}
static int redirect(lua_State*L){auto*s=fromDesc(L,1),*t=fromDesc(L,2);if(!s||!t)return luaL_error(L,"Hook.redirect method not found");if(Il2cpp::GetMethodParamCount(s)!=Il2cpp::GetMethodParamCount(t))return luaL_error(L,"Hook.redirect arg mismatch");return install(L,s,t->methodPointer);}

static int azPrint(lua_State*L){int n=lua_gettop(L);std::string z;lua_getglobal(L,"tostring");for(int i=1;i<=n;i++){lua_pushvalue(L,-1);lua_pushvalue(L,i);lua_call(L,1,1);if(i>1)z+='\t';if(const char*s=lua_tostring(L,-1))z+=s;lua_pop(L,1);}lua_pop(L,1);out(z);logf("LUA %s",z.c_str());return 0;}
static int alert(lua_State*L){const char*s=luaL_tolstring(L,1,nullptr);out(std::string("[ALERT] ")+(s?s:""));lua_pop(L,1);return 0;}
static int sleepms(lua_State*L){lua_Integer m=luaL_checkinteger(L,1);if(m>0)usleep((useconds_t)std::min<lua_Integer>(m,600000)*1000);return 0;}
static int noop(lua_State*){return 0;}
static void reg(lua_State*L){
 luaL_newmetatable(L,"AZ.Obj");lua_pushcfunction(L,objIndex);lua_setfield(L,-2,"__index");lua_pushcfunction(L,objgc);lua_setfield(L,-2,"__gc");lua_pushcfunction(L,objstr);lua_setfield(L,-2,"__tostring");lua_pop(L,1);
 luaL_newmetatable(L,"AZ.Cls");lua_pushcfunction(L,clsIndex);lua_setfield(L,-2,"__index");lua_pushcfunction(L,clsstr);lua_setfield(L,-2,"__tostring");lua_pop(L,1);
 lua_newtable(L);lua_pushcfunction(L,classFrom);lua_setfield(L,-2,"fromName");lua_setglobal(L,"Class");
 lua_newtable(L);lua_pushcfunction(L,callExact);lua_setfield(L,-2,"exact");lua_pushcfunction(L,callExact);lua_setfield(L,-2,"default");lua_setglobal(L,"Call");
 lua_newtable(L);lua_pushcfunction(L,bridge);lua_setfield(L,-2,"bridge");lua_pushcfunction(L,redirect);lua_setfield(L,-2,"redirect");lua_pushcfunction(L,unbridge);lua_setfield(L,-2,"unbridge");lua_setglobal(L,"Hook");
 lua_newtable(L);lua_pushcfunction(L,arrayGet);lua_setfield(L,-2,"GetValue");lua_pushcfunction(L,arrayLen);lua_setfield(L,-2,"Length");lua_setglobal(L,"Array");
 lua_newtable(L);lua_pushcfunction(L,alert);lua_setfield(L,-2,"alert");lua_pushcfunction(L,sleepms);lua_setfield(L,-2,"sleep");lua_pushcfunction(L,noop);lua_setfield(L,-2,"setVisible");lua_setglobal(L,"gg");
 lua_pushcfunction(L,azPrint);lua_setglobal(L,"print");lua_getglobal(L,"load");lua_setglobal(L,"loadstring");lua_pushliteral(L,"AZ ScriptCore 0.7");lua_setglobal(L,"AZ_VERSION");
}
static bool safe(const std::string&s){return!s.empty()&&s.size()<240&&s.find("..")==std::string::npos&&s.find('/')==std::string::npos&&s.find('\\')==std::string::npos&&s.size()>4&&s.substr(s.size()-4)==".lua";}
static const char* coreAbi(){
#if defined(__aarch64__)
 return "arm64";
#elif defined(__arm__)
 return "arm32";
#elif defined(__x86_64__)
 return "x86_64";
#else
 return "x86";
#endif
}
static bool queueScriptOnUnityMain(const std::string&name){
 g_stopRequested.store(false,std::memory_order_relaxed);
 {
  std::lock_guard<std::mutex>lk(g_runMetaMu);
  g_runningName=name; g_runningStartMs=0;
 }
 writeStatus("QUEUED",name,0,0,"");
 if(!waitMainDispatcher(5000)){
  logf("UnityMain script queue unavailable name=%s",name.c_str());
  writeStatus("ERROR",name,0,0,"UnityMain dispatcher unavailable");
  return false;
 }
 MainScriptTask task;task.name=name;
 {
  std::lock_guard<std::mutex>lk(g_scriptQueueMu);
  g_scriptQueue.push_back(&task);
 }
 logf("UnityMain script queued name=%s",name.c_str());
 // No SynchronizationContext.Post is required here: Exec is already our V7-style
 // UnityMain bootstrap and runs every player-loop frame.
 std::unique_lock<std::mutex>lk(task.mu);
 task.cv.wait(lk,[&]{return task.done;});
 return true;
}

static void run(const std::string&name){
 std::string f=safe(name)?g_base+"/"+name:g_base+"/script.lua";
 long long start=monoMs();
 {
  std::lock_guard<std::mutex>lk(g_runMetaMu);
  g_runningName=name; g_runningStartMs=start;
 }

 if(g_stopRequested.load(std::memory_order_relaxed)){
  out("[AZ ScriptCore] STOPPED before start");
  writeStatus("STOPPED",name,start,0,"stopped by user");
  g_stopRequested.store(false,std::memory_order_relaxed);
  std::lock_guard<std::mutex>lk(g_runMetaMu);g_runningName.clear();g_runningStartMs=0;
  return;
 }

 writeStatus("RUNNING",name,start,0,"");
 writeText(OUT(),"[AZ ScriptCore V0.7] RUN | "+f+"\n");
 out("[ENV] package="+g_pkg+" | abi="+coreAbi()+" | images="+std::to_string(Il2cpp::GetImagesFresh().size())+" | gchandle="+(Il2cpp::GcHandleApiResolved()?"OK":"MISSING"));
 out("[API] Class.fromName | findObjectsFresh | Field | Call.exact/default | Hook | Array | gg");
 out("[OBJ] V7 ownership | userdata=raw+handle0 | call-result roots=external");
 out("[CTRL] stop=enabled | cooperative Lua interrupt");

 lua_sethook(G,luaStopHook,LUA_MASKCOUNT,2000);
 int rc=luaL_loadfile(G,f.c_str());
 if(rc==LUA_OK)rc=lua_pcall(G,0,LUA_MULTRET,0);
 lua_sethook(G,nullptr,0,0);

 long long elapsed=monoMs()-start;
 bool stopped=g_stopRequested.load(std::memory_order_relaxed);
 const char*err=(rc!=LUA_OK)?lua_tostring(G,-1):nullptr;
 if(err&&strstr(err,"__AZ_STOP_REQUESTED__"))stopped=true;

 if(stopped){
  if(rc!=LUA_OK&&lua_gettop(G)>0)lua_pop(G,1);
  char b[128];snprintf(b,sizeof(b),"[AZ ScriptCore] STOPPED | elapsed=%.3fs",elapsed/1000.0);
  out(b);
  writeStatus("STOPPED",name,start,elapsed,"stopped by user");
 }else if(rc!=LUA_OK){
  std::string msg=err?err:"unknown";
  out(std::string("ERROR | ")+msg);
  if(lua_gettop(G)>0)lua_pop(G,1);
  writeStatus("ERROR",name,start,elapsed,msg);
 }else{
  char b[128];snprintf(b,sizeof(b),"[AZ ScriptCore 0.7] DONE | elapsed=%.3fs",elapsed/1000.0);
  out(b);
  writeStatus("DONE",name,start,elapsed,"");
 }

 g_stopRequested.store(false,std::memory_order_relaxed);
 {
  std::lock_guard<std::mutex>lk(g_runMetaMu);
  g_runningName.clear();g_runningStartMs=0;
 }
 lua_settop(G,0);
 lua_gc(G,LUA_GCCOLLECT,0);
}
void worker(){ensureDir();writeStatus("INIT");G=luaL_newstate();if(!G){writeStatus("ERROR","",0,0,"Lua init failed");return;}luaL_openlibs(G);reg(G);installMainThreadBootstrap();writeStatus("READY");logf("Lua 5.4 READY | Class Call Hook Array gg | UnityMain bootstrap=%d",g_mainHookInstalled.load()?1:0);for(;;){if(access(REQ(),F_OK)==0){std::string q=readText(REQ(),512);unlink(REQ());while(!q.empty()&&(q.back()=='\n'||q.back()=='\r'||q.back()==' '||q.back()=='\t'))q.pop_back();if(!queueScriptOnUnityMain(q)){out("ERROR | UnityMain script dispatcher unavailable");}}usleep(100000);}}
}
extern "C" void az_lua_worker(){AZLua::worker();}
extern "C" void az_lua_request_stop(){AZLua::requestStop();}
