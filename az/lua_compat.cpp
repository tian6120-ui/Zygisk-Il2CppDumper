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
#include "Il2cpp/Il2cpp.h"
#include "Il2cpp/il2cpp-class.h"
#include "Dobby/dobby.h"
#include "Includes/Logger.h"
extern "C" {
#include "azlua/lua.h"
#include "azlua/lauxlib.h"
#include "azlua/lualib.h"
}

namespace AZLua {
static constexpr const char *BASE="/data/user/0/com.springgames.archercastle/files/AZTool";
static constexpr const char *REQ="/data/user/0/com.springgames.archercastle/files/AZTool/script.req";
static constexpr const char *OUT="/data/user/0/com.springgames.archercastle/files/AZTool/script.out";
static constexpr const char *STATUS="/data/user/0/com.springgames.archercastle/files/AZTool/script.status";
struct Obj { Il2CppObject *raw{}; uint32_t handle{}; };
struct Cls { Il2CppClass *klass{}; };
struct HookRec { int id{}; MethodInfo *method{}; void *target{}; void *orig{}; };
static lua_State *G=nullptr;
static std::mutex hookMu;
static std::unordered_map<int,HookRec> hooks;
static std::atomic<int> nextHook{1};

static void logf(const char *fmt,...){
 char b[1024]; va_list ap; va_start(ap,fmt); vsnprintf(b,sizeof(b),fmt,ap); va_end(ap);
 __android_log_print(ANDROID_LOG_INFO,"AZL07","%s",b);
 FILE *f=fopen("/data/local/tmp/AZTool/core.log","a"); if(f){fprintf(f,"[AZL07] %s\n",b);fclose(f);}
}
static void ensureDir(){mkdir("/data/user/0/com.springgames.archercastle/files",0700);mkdir(BASE,0700);}
static void writeText(const char *p,const std::string&s){FILE*f=fopen(p,"wb");if(!f)return;fwrite(s.data(),1,s.size(),f);fclose(f);}
static std::string readText(const char*p,size_t lim=1048576){FILE*f=fopen(p,"rb");if(!f)return{};std::string s;char b[4096];while(!feof(f)&&s.size()<lim){size_t n=fread(b,1,sizeof(b),f);if(!n)break;if(s.size()+n>lim)n=lim-s.size();s.append(b,n);}fclose(f);return s;}
static void out(const std::string&s){FILE*f=fopen(OUT,"ab");if(!f)return;fwrite(s.data(),1,s.size(),f);fwrite("\n",1,1,f);fclose(f);}
static std::string tname(Il2CppType*t){const char*n=t?Il2cpp::GetTypeName(t):nullptr;return n?n:"";}
static std::string shortn(std::string s){if(s.rfind("System.",0)==0)s.erase(0,7);return s;}
static bool teq(std::string a,std::string b){return a==b||shortn(a)==shortn(b);}
static Obj* ckobj(lua_State*L,int i){return(Obj*)luaL_checkudata(L,i,"AZ.Obj");}
static Cls* ckcls(lua_State*L,int i){return(Cls*)luaL_checkudata(L,i,"AZ.Cls");}
static Il2CppObject* resolve(Obj*u){if(!u)return nullptr;if(u->handle){auto*p=Il2cpp::GC::GetHandleTarget(u->handle);if(p){u->raw=p;return p;}}return u->raw;}
static int pushObj(lua_State*L,Il2CppObject*o){if(!o){lua_pushnil(L);return 1;}auto*u=(Obj*)lua_newuserdatauv(L,sizeof(Obj),0);new(u)Obj{};u->raw=o;if(Il2cpp::GcHandleApiResolved())u->handle=Il2cpp::GC::NewHandle(o,false);luaL_getmetatable(L,"AZ.Obj");lua_setmetatable(L,-2);return 1;}
static int pushCls(lua_State*L,Il2CppClass*k){if(!k){lua_pushnil(L);return 1;}auto*u=(Cls*)lua_newuserdatauv(L,sizeof(Cls),0);u->klass=k;luaL_getmetatable(L,"AZ.Cls");lua_setmetatable(L,-2);return 1;}
static int objgc(lua_State*L){auto*u=(Obj*)luaL_testudata(L,1,"AZ.Obj");if(u&&u->handle){Il2cpp::GC::FreeHandle(u->handle);u->handle=0;}return 0;}
static int objstr(lua_State*L){auto*o=resolve(ckobj(L,1));if(!o){lua_pushliteral(L,"<managed:null>");return 1;}auto*k=Il2cpp::GetObjectClass(o);lua_pushfstring(L,"<managed:%s@%p>",k?k->getName():"?",o);return 1;}
static int clsstr(lua_State*L){auto*c=ckcls(L,1);lua_pushfstring(L,"<class:%s>",c&&c->klass?c->klass->getFullName().c_str():"null");return 1;}
static bool isEnum(Il2CppType*t){auto*k=t?t->getClass():nullptr;return k&&Il2cpp::GetClassIsEnum(k);}

static int pushManaged(lua_State*L,Il2CppObject*o,Il2CppType*t){
 std::string n=tname(t);if(n=="System.Void"||n=="Void"){lua_pushnil(L);return 1;}if(!o){lua_pushnil(L);return 1;}
 if(n=="System.String"||n=="String"){lua_pushstring(L,((Il2CppString*)o)->to_string().c_str());return 1;}
 void*p=Il2cpp::GetUnboxedValue(o);
 if(n=="System.Boolean"||n=="Boolean"){lua_pushboolean(L,p?*(bool*)p:false);return 1;}
 if(n=="System.SByte"){lua_pushinteger(L,p?*(int8_t*)p:0);return 1;} if(n=="System.Byte"){lua_pushinteger(L,p?*(uint8_t*)p:0);return 1;}
 if(n=="System.Int16"){lua_pushinteger(L,p?*(int16_t*)p:0);return 1;} if(n=="System.UInt16"||n=="System.Char"){lua_pushinteger(L,p?*(uint16_t*)p:0);return 1;}
 if(n=="System.Int32"||n=="Int32"){lua_pushinteger(L,p?*(int32_t*)p:0);return 1;} if(n=="System.UInt32"||n=="UInt32"){lua_pushinteger(L,p?*(uint32_t*)p:0);return 1;}
 if(n=="System.Int64"||n=="Int64"){lua_pushinteger(L,p?(lua_Integer)*(int64_t*)p:0);return 1;} if(n=="System.UInt64"||n=="UInt64"){lua_pushinteger(L,p?(lua_Integer)*(uint64_t*)p:0);return 1;}
 if(n=="System.Single"||n=="Single"){lua_pushnumber(L,p?*(float*)p:0);return 1;} if(n=="System.Double"||n=="Double"){lua_pushnumber(L,p?*(double*)p:0);return 1;}
 if(isEnum(t)){auto*b=Il2cpp::GetEnumBaseType(t->getClass());std::string bn=tname(b);if(bn=="System.Int64")lua_pushinteger(L,p?(lua_Integer)*(int64_t*)p:0);else if(bn=="System.UInt64")lua_pushinteger(L,p?(lua_Integer)*(uint64_t*)p:0);else if(bn=="System.Int16")lua_pushinteger(L,p?*(int16_t*)p:0);else if(bn=="System.UInt16")lua_pushinteger(L,p?*(uint16_t*)p:0);else if(bn=="System.Byte")lua_pushinteger(L,p?*(uint8_t*)p:0);else if(bn=="System.SByte")lua_pushinteger(L,p?*(int8_t*)p:0);else if(bn=="System.UInt32")lua_pushinteger(L,p?*(uint32_t*)p:0);else lua_pushinteger(L,p?*(int32_t*)p:0);return 1;}
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
static bool desc(lua_State*L,int i,std::string&d,std::string&n,std::vector<std::string>&p){
 if(!lua_istable(L,i))return false;lua_getfield(L,i,"declaring");if(const char*s=lua_tostring(L,-1))d=s;lua_pop(L,1);lua_getfield(L,i,"name");if(const char*s=lua_tostring(L,-1))n=s;lua_pop(L,1);lua_getfield(L,i,"params");if(lua_istable(L,-1)){lua_Integer z=lua_rawlen(L,-1);for(lua_Integer x=1;x<=z;x++){lua_geti(L,-1,x);const char*s=lua_tostring(L,-1);p.emplace_back(s?s:"");lua_pop(L,1);}}lua_pop(L,1);return!d.empty()&&!n.empty();
}
static MethodInfo* fromDesc(lua_State*L,int i){std::string d,n;std::vector<std::string>p;if(!desc(L,i,d,n,p))return nullptr;auto*k=Il2cpp::FindClass(d.c_str());return k?findMethod(k,n,(int)p.size(),&p):nullptr;}
static int invoke(lua_State*L,MethodInfo*m,Il2CppObject*recv,int first,int ac){
 if(!m)return luaL_error(L,"method not found");int n=(int)Il2cpp::GetMethodParamCount(m);if(n!=ac)return luaL_error(L,"arg count need=%d got=%d",n,ac);std::vector<Il2CppObject*>a(n);
 for(int j=0;j<n;j++){auto*t=Il2cpp::GetMethodParam(m,j);a[j]=box(L,first+j,t);if(!lua_isnil(L,first+j)&&!a[j]&&!luaL_testudata(L,first+j,"AZ.Obj"))return luaL_error(L,"marshal arg %d as %s failed",j+1,tname(t).c_str());}
 Il2CppException*e=nullptr;auto*r=Il2cpp::RuntimeInvokeConvertArgs(m,recv,a.empty()?nullptr:a.data(),n,&e);if(e)return luaL_error(L,"managed exception in %s",Il2cpp::GetMethodName(m));return pushManaged(L,r,Il2cpp::GetMethodReturnType(m));
}

static int classFrom(lua_State*L){return pushCls(L,Il2cpp::FindClass(luaL_checkstring(L,1)));}
static int findObjects(lua_State*L){auto*c=ckcls(L,1);auto v=(c&&c->klass)?Il2cpp::GC::FindObjects(c->klass):std::vector<Il2CppObject*>{};lua_createtable(L,(int)v.size(),0);int i=1;for(auto*o:v){pushObj(L,o);lua_seti(L,-2,i++);}return 1;}
static int clsIndex(lua_State*L){auto*c=ckcls(L,1);const char*k=luaL_checkstring(L,2);if(!strcmp(k,"findObjectsFresh")||!strcmp(k,"findObjects")){lua_pushcfunction(L,findObjects);return 1;}if(!strcmp(k,"name")){lua_pushstring(L,c&&c->klass?c->klass->getFullName().c_str():"");return 1;}lua_pushnil(L);return 1;}
static int getField(lua_State*L){auto*o=resolve(ckobj(L,1));const char*n=luaL_checkstring(L,2);if(!o){lua_pushnil(L);return 1;}auto*k=Il2cpp::GetObjectClass(o);auto*f=k?k->getFieldInHierarchy(n):nullptr;if(!f){lua_pushnil(L);return 1;}return pushManaged(L,Il2cpp::GetFieldValueObject(o,f),f->getType());}
static int setField(lua_State*L){auto*o=resolve(ckobj(L,1));const char*n=luaL_checkstring(L,2);if(!o)return luaL_error(L,"null object");auto*k=Il2cpp::GetObjectClass(o);auto*f=k?k->getFieldInHierarchy(n):nullptr;if(!f)return luaL_error(L,"field not found: %s",n);auto*b=box(L,3,f->getType());if(f->getType()->isObject()||f->getType()->isArray())Il2cpp::SetFieldValueObject(o,f,b);else{void*p=b?Il2cpp::GetUnboxedValue(b):nullptr;if(!p)return luaL_error(L,"field marshal failed");Il2cpp::SetFieldValue(o,f,p);}lua_pushboolean(L,1);return 1;}
static int dynCall(lua_State*L){const char*n=lua_tostring(L,lua_upvalueindex(1));auto*o=resolve(ckobj(L,1));if(!o)return luaL_error(L,"null object");int ac=lua_gettop(L)-1;auto*m=findMethod(Il2cpp::GetObjectClass(o),n?n:"",ac);if(!m)return luaL_error(L,"method not found: %s",n?n:"?");return invoke(L,m,o,2,ac);}
static int objIndex(lua_State*L){auto*o=resolve(ckobj(L,1));const char*k=luaL_checkstring(L,2);if(!strcmp(k,"getField")){lua_pushcfunction(L,getField);return 1;}if(!strcmp(k,"setField")){lua_pushcfunction(L,setField);return 1;}if(!o){lua_pushnil(L);return 1;}auto*c=Il2cpp::GetObjectClass(o);if(auto*f=c?c->getFieldInHierarchy(k):nullptr)return pushManaged(L,Il2cpp::GetFieldValueObject(o,f),f->getType());if(strncmp(k,"get_",4)&&strncmp(k,"set_",4)){std::string g="get_";g+=k;if(auto*m=findMethod(c,g,0))return invoke(L,m,o,0,0);}lua_pushstring(L,k);lua_pushcclosure(L,dynCall,1);return 1;}
static int callExact(lua_State*L){auto*m=fromDesc(L,1);if(!m)return luaL_error(L,"Call.exact method not found");Il2CppObject*r=nullptr;if(luaL_testudata(L,2,"AZ.Obj"))r=resolve(ckobj(L,2));return invoke(L,m,r,3,lua_gettop(L)-2);}
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
static void run(const std::string&name){std::string f=safe(name)?std::string(BASE)+"/"+name:std::string(BASE)+"/script.lua";writeText(OUT,"[AZ ScriptCore 0.7] RUN | "+f+"\n");writeText(STATUS,"RUNNING");int rc=luaL_loadfile(G,f.c_str());if(rc==LUA_OK)rc=lua_pcall(G,0,LUA_MULTRET,0);if(rc!=LUA_OK){const char*e=lua_tostring(G,-1);out(std::string("ERROR | ")+(e?e:"unknown"));lua_pop(G,1);writeText(STATUS,"ERROR");}else{out("[AZ ScriptCore 0.7] DONE");writeText(STATUS,"DONE");}lua_settop(G,0);lua_gc(G,LUA_GCCOLLECT,0);}
void worker(){ensureDir();writeText(STATUS,"INIT");G=luaL_newstate();if(!G){writeText(STATUS,"LUA_INIT_FAILED");return;}luaL_openlibs(G);reg(G);writeText(STATUS,"READY");logf("Lua 5.4 READY | Class Call Hook Array gg");for(;;){if(access(REQ,F_OK)==0){std::string q=readText(REQ,512);unlink(REQ);while(!q.empty()&&(q.back()=='\n'||q.back()=='\r'||q.back()==' '||q.back()=='\t'))q.pop_back();run(q);}usleep(100000);}}
}
extern "C" void az_lua_worker(){AZLua::worker();}
