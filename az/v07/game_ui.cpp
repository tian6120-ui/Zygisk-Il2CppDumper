#include "game_ui.h"
#include <android/input.h>
#include <android/log.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <algorithm>
#include <ctime>

#include "imgui.h"
#include "backends/imgui_impl_opengl3.h"
#include "backends/imgui_impl_android.h"
#include "Dobby/dobby.h"

#define AZTAG "AZUI07"
#define AZLOGI(...) __android_log_print(ANDROID_LOG_INFO,AZTAG,__VA_ARGS__)
#define AZLOGE(...) __android_log_print(ANDROID_LOG_ERROR,AZTAG,__VA_ARGS__)

namespace {
JavaVM* g_vm=nullptr;
jobject g_context=nullptr;
std::mutex g_jni_mu;
std::atomic<bool> g_activate{false};
std::atomic<bool> g_hook_started{false};

using SwapFn=EGLBoolean(*)(EGLDisplay,EGLSurface);
SwapFn g_orig_swap=nullptr;
using AQGetEventFn=int32_t(*)(AInputQueue*,AInputEvent**);
AQGetEventFn g_orig_aq_get=nullptr;
using ConsumeFn=int32_t(*)(void*,void*,bool,int64_t,uint32_t*,AInputEvent**);
ConsumeFn g_orig_consume=nullptr;

bool g_imgui=false;
bool g_open=true;
bool g_auto_scroll=true;
int g_w=0,g_h=0;
timespec g_last{};
std::vector<char> g_script(256*1024,0);
std::string g_output;
std::string g_status="WAITING";
std::string g_base;
std::string g_last_script_path;
std::string g_req_path;
std::string g_out_path;
std::string g_status_path;
double g_last_poll=0.0;

std::string procPkg(){
 FILE*f=fopen("/proc/self/cmdline","rb"); if(!f)return "unknown";
 char b[256]{};size_t n=fread(b,1,sizeof(b)-1,f);fclose(f);
 std::string s(b,n);auto z=s.find('\0');if(z!=std::string::npos)s.resize(z);
 auto c=s.find(':');if(c!=std::string::npos)s.resize(c);
 return s.empty()?"unknown":s;
}
void ensurePaths(){
 if(!g_base.empty())return;
 auto p=procPkg();
 std::string files="/data/user/0/"+p+"/files";
 g_base=files+"/AZTool";
 mkdir(files.c_str(),0700);mkdir(g_base.c_str(),0700);
 g_last_script_path=g_base+"/last.lua";
 g_req_path=g_base+"/script.req";
 g_out_path=g_base+"/script.out";
 g_status_path=g_base+"/script.status";
}
std::string readFile(const std::string&p,size_t limit=2*1024*1024){
 FILE*f=fopen(p.c_str(),"rb");if(!f)return{};
 std::string s;char b[4096];
 while(!feof(f)&&s.size()<limit){size_t n=fread(b,1,sizeof(b),f);if(!n)break;if(s.size()+n>limit)n=limit-s.size();s.append(b,n);}
 fclose(f);return s;
}
bool writeFile(const std::string&p,const std::string&s){
 FILE*f=fopen(p.c_str(),"wb");if(!f)return false;size_t n=fwrite(s.data(),1,s.size(),f);fclose(f);return n==s.size();
}
void appendDiag(const std::string&s){
 ensurePaths();FILE*f=fopen(g_out_path.c_str(),"ab");if(f){fwrite(s.data(),1,s.size(),f);fwrite("\n",1,1,f);fclose(f);}
}
double nowSec(){timespec t{};clock_gettime(CLOCK_MONOTONIC,&t);return double(t.tv_sec)+double(t.tv_nsec)/1e9;}

JNIEnv* envFor(bool&detach){
 detach=false;if(!g_vm)return nullptr;JNIEnv*e=nullptr;
 if(g_vm->GetEnv((void**)&e,JNI_VERSION_1_6)==JNI_OK)return e;
#if defined(__ANDROID__)
 if(g_vm->AttachCurrentThread(&e,nullptr)==JNI_OK){detach=true;return e;}
#endif
 return nullptr;
}
jobject localContext(JNIEnv*e){
 std::lock_guard<std::mutex>lk(g_jni_mu);
 return (e&&g_context)?e->NewLocalRef(g_context):nullptr;
}
void clearExc(JNIEnv*e){if(e&&e->ExceptionCheck()){e->ExceptionClear();}}

std::string clipboardGet(){
 bool det=false;JNIEnv*e=envFor(det);if(!e)return{};
 jobject ctx=localContext(e);if(!ctx){if(det)g_vm->DetachCurrentThread();return{};}
 std::string out;
 jclass cc=e->GetObjectClass(ctx);
 jmethodID gss=e->GetMethodID(cc,"getSystemService","(Ljava/lang/String;)Ljava/lang/Object;");
 jstring svc=e->NewStringUTF("clipboard");
 jobject cm=gss?e->CallObjectMethod(ctx,gss,svc):nullptr;clearExc(e);
 if(cm){
   jclass cmc=e->GetObjectClass(cm);
   jmethodID gp=e->GetMethodID(cmc,"getPrimaryClip","()Landroid/content/ClipData;");
   jobject clip=gp?e->CallObjectMethod(cm,gp):nullptr;clearExc(e);
   if(clip){
     jclass clc=e->GetObjectClass(clip);
     jmethodID gc=e->GetMethodID(clc,"getItemCount","()I");
     jint count=gc?e->CallIntMethod(clip,gc):0;clearExc(e);
     if(count>0){
       jmethodID gi=e->GetMethodID(clc,"getItemAt","(I)Landroid/content/ClipData$Item;");
       jobject item=gi?e->CallObjectMethod(clip,gi,0):nullptr;clearExc(e);
       if(item){
         jclass ic=e->GetObjectClass(item);
         jmethodID ct=e->GetMethodID(ic,"coerceToText","(Landroid/content/Context;)Ljava/lang/CharSequence;");
         jobject cs=ct?e->CallObjectMethod(item,ct,ctx):nullptr;clearExc(e);
         if(cs){
           jclass oc=e->FindClass("java/lang/Object");
           jmethodID ts=e->GetMethodID(oc,"toString","()Ljava/lang/String;");
           jstring js=(jstring)e->CallObjectMethod(cs,ts);clearExc(e);
           if(js){const char*p=e->GetStringUTFChars(js,nullptr);if(p){out=p;e->ReleaseStringUTFChars(js,p);}e->DeleteLocalRef(js);}
           e->DeleteLocalRef(oc);e->DeleteLocalRef(cs);
         }
         e->DeleteLocalRef(ic);e->DeleteLocalRef(item);
       }
     }
     e->DeleteLocalRef(clc);e->DeleteLocalRef(clip);
   }
   e->DeleteLocalRef(cmc);e->DeleteLocalRef(cm);
 }
 e->DeleteLocalRef(svc);e->DeleteLocalRef(cc);e->DeleteLocalRef(ctx);
 if(det)g_vm->DetachCurrentThread();
 return out;
}
bool clipboardSet(const std::string&s){
 bool det=false;JNIEnv*e=envFor(det);if(!e)return false;
 jobject ctx=localContext(e);if(!ctx){if(det)g_vm->DetachCurrentThread();return false;}
 bool ok=false;
 jclass cc=e->GetObjectClass(ctx);
 jmethodID gss=e->GetMethodID(cc,"getSystemService","(Ljava/lang/String;)Ljava/lang/Object;");
 jstring svc=e->NewStringUTF("clipboard");
 jobject cm=gss?e->CallObjectMethod(ctx,gss,svc):nullptr;clearExc(e);
 jclass clip=e->FindClass("android/content/ClipData");
 if(cm&&clip){
   jmethodID np=e->GetStaticMethodID(clip,"newPlainText","(Ljava/lang/CharSequence;Ljava/lang/CharSequence;)Landroid/content/ClipData;");
   jstring label=e->NewStringUTF("AZ Output");
   jstring txt=e->NewStringUTF(s.c_str());
   jobject cd=np?e->CallStaticObjectMethod(clip,np,label,txt):nullptr;clearExc(e);
   if(cd){
     jclass cmc=e->GetObjectClass(cm);
     jmethodID sp=e->GetMethodID(cmc,"setPrimaryClip","(Landroid/content/ClipData;)V");
     if(sp){e->CallVoidMethod(cm,sp,cd);clearExc(e);ok=true;}
     e->DeleteLocalRef(cmc);e->DeleteLocalRef(cd);
   }
   e->DeleteLocalRef(label);e->DeleteLocalRef(txt);
 }
 if(clip)e->DeleteLocalRef(clip);if(cm)e->DeleteLocalRef(cm);
 e->DeleteLocalRef(svc);e->DeleteLocalRef(cc);e->DeleteLocalRef(ctx);
 if(det)g_vm->DetachCurrentThread();
 return ok;
}

void pollFiles(){
 double n=nowSec();if(n-g_last_poll<0.20)return;g_last_poll=n;
 ensurePaths();
 g_status=readFile(g_status_path,4096);while(!g_status.empty()&&(g_status.back()=='\n'||g_status.back()=='\r'))g_status.pop_back();
 g_output=readFile(g_out_path);
}
void loadLast(){
 ensurePaths();auto s=readFile(g_last_script_path,g_script.size()-1);
 memset(g_script.data(),0,g_script.size());if(!s.empty()){memcpy(g_script.data(),s.data(),std::min(s.size(),g_script.size()-1));}
}
void saveLast(){
 ensurePaths();writeFile(g_last_script_path,std::string(g_script.data()));
}
void requestRun(){
 saveLast();writeFile(g_req_path,"last.lua\n");
}
void setupStyle(){
 ImGuiStyle&s=ImGui::GetStyle();s.WindowRounding=10;s.ChildRounding=8;s.FrameRounding=7;s.PopupRounding=8;s.ScrollbarRounding=7;s.GrabRounding=7;
 s.ScaleAllSizes(1.35f);
 ImVec4*c=s.Colors;
 c[ImGuiCol_WindowBg]=ImVec4(.045f,.05f,.06f,.96f);
 c[ImGuiCol_TitleBg]=ImVec4(.12f,.02f,.03f,1);
 c[ImGuiCol_TitleBgActive]=ImVec4(.48f,.02f,.05f,1);
 c[ImGuiCol_Button]=ImVec4(.34f,.035f,.065f,1);
 c[ImGuiCol_ButtonHovered]=ImVec4(.60f,.045f,.09f,1);
 c[ImGuiCol_ButtonActive]=ImVec4(.82f,.055f,.11f,1);
 c[ImGuiCol_Header]=ImVec4(.36f,.035f,.07f,.95f);
}
void setupImGui(){
 if(g_imgui)return;
 IMGUI_CHECKVERSION();ImGui::CreateContext();ImGuiIO&io=ImGui::GetIO();io.IniFilename=nullptr;io.DisplaySize=ImVec2((float)g_w,(float)g_h);
 ImGui_ImplOpenGL3_Init("#version 300 es");setupStyle();clock_gettime(CLOCK_MONOTONIC,&g_last);loadLast();g_imgui=true;
 appendDiag("[AZ V0.7 UI] READY | package="+procPkg());
}
void draw(){
 pollFiles();ImGuiIO&io=ImGui::GetIO();io.DisplaySize=ImVec2((float)g_w,(float)g_h);
 timespec n{};clock_gettime(CLOCK_MONOTONIC,&n);double dt=(n.tv_sec-g_last.tv_sec)+(n.tv_nsec-g_last.tv_nsec)/1e9;if(dt<=0||dt>.25)dt=1.0/60.0;io.DeltaTime=(float)dt;g_last=n;
 ImGui_ImplOpenGL3_NewFrame();ImGui::NewFrame();

 ImGui::SetNextWindowPos(ImVec2(24,110),ImGuiCond_Once);ImGui::SetNextWindowSize(ImVec2(88,88),ImGuiCond_Always);
 if(ImGui::Begin("##AZFloat",nullptr,ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoSavedSettings)){
   ImGui::SetCursorPos(ImVec2(8,8));if(ImGui::Button("AZ",ImVec2(70,70)))g_open=!g_open;
 }ImGui::End();

 if(g_open){
   ImGui::SetNextWindowPos(ImVec2(125,70),ImGuiCond_Once);
   ImGui::SetNextWindowSize(ImVec2(std::max(520.0f,g_w*.62f),std::max(520.0f,g_h*.72f)),ImGuiCond_Once);
   if(ImGui::Begin("AZ ScriptCore V0.7",&g_open,ImGuiWindowFlags_NoSavedSettings)){
     ImGui::Text("Status: %s",g_status.empty()?"WAITING":g_status.c_str());ImGui::SameLine();
#if defined(__aarch64__)
     ImGui::TextDisabled("| ABI arm64");
#elif defined(__arm__)
     ImGui::TextDisabled("| ABI arm32");
#elif defined(__x86_64__)
     ImGui::TextDisabled("| ABI x86_64");
#else
     ImGui::TextDisabled("| ABI x86");
#endif
     if(ImGui::BeginTabBar("tabs")){
       if(ImGui::BeginTabItem("Script")){
         if(ImGui::Button("Paste & Run")){
           auto s=clipboardGet();if(!s.empty()){memset(g_script.data(),0,g_script.size());memcpy(g_script.data(),s.data(),std::min(s.size(),g_script.size()-1));requestRun();}
           else appendDiag("[AZ UI] clipboard empty / unavailable");
         }ImGui::SameLine();
         if(ImGui::Button("Run"))requestRun();ImGui::SameLine();
         if(ImGui::Button("Save"))saveLast();ImGui::SameLine();
         if(ImGui::Button("Load Last"))loadLast();ImGui::SameLine();
         if(ImGui::Button("Clear Editor"))memset(g_script.data(),0,g_script.size());
         ImGui::Separator();
         ImGui::InputTextMultiline("##lua",g_script.data(),g_script.size(),ImVec2(-1,-1),ImGuiInputTextFlags_AllowTabInput);
         ImGui::EndTabItem();
       }
       if(ImGui::BeginTabItem("Output")){
         if(ImGui::Button("Copy All")){if(!clipboardSet(g_output))appendDiag("[AZ UI] copy failed: context unavailable");}
         ImGui::SameLine();
         if(ImGui::Button("Clear")){writeFile(g_out_path,"");g_output.clear();}
         ImGui::SameLine();
         if(ImGui::Button("Refresh")){g_last_poll=0;pollFiles();}
         ImGui::SameLine();ImGui::Checkbox("Auto-scroll",&g_auto_scroll);
         ImGui::Separator();
         ImGui::BeginChild("out",ImVec2(0,0),true,ImGuiWindowFlags_HorizontalScrollbar);
         ImGui::TextUnformatted(g_output.c_str(),g_output.c_str()+g_output.size());
         if(g_auto_scroll&&ImGui::GetScrollY()>=ImGui::GetScrollMaxY()-40.0f)ImGui::SetScrollHereY(1.0f);
         ImGui::EndChild();ImGui::EndTabItem();
       }
       if(ImGui::BeginTabItem("Info")){
         ImGui::TextWrapped("Test flow: copy Lua -> Paste & Run -> Output -> Copy All.");
         ImGui::Text("Package: %s",procPkg().c_str());
         ImGui::TextWrapped("Script: %s",g_last_script_path.c_str());
         ImGui::TextWrapped("Output: %s",g_out_path.c_str());
         if(ImGui::Button("Copy Diagnostics")){
           std::string d="AZ ScriptCore V0.7\npackage="+procPkg()+"\nstatus="+g_status+"\n"+g_output;clipboardSet(d);
         }
         ImGui::EndTabItem();
       }
       ImGui::EndTabBar();
     }
   }ImGui::End();
 }
 ImGui::Render();ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}
EGLBoolean swapHook(EGLDisplay d,EGLSurface s){
 EGLint w=0,h=0;eglQuerySurface(d,s,EGL_WIDTH,&w);eglQuerySurface(d,s,EGL_HEIGHT,&h);if(w>0&&h>0){g_w=w;g_h=h;}
 if(!g_imgui&&g_w>0&&g_h>0)setupImGui();if(g_imgui)draw();return g_orig_swap?g_orig_swap(d,s):EGL_FALSE;
}
int32_t aqHook(AInputQueue*q,AInputEvent**e){int32_t r=g_orig_aq_get?g_orig_aq_get(q,e):-1;if(r>=0&&e&&*e&&ImGui::GetCurrentContext())ImGui_ImplAndroid_HandleInputEvent(*e);return r;}
int32_t consumeHook(void*a,void*b,bool c,int64_t d,uint32_t*e,AInputEvent**ev){int32_t r=g_orig_consume?g_orig_consume(a,b,c,d,e,ev):-1;if(r==0&&ev&&*ev&&ImGui::GetCurrentContext())ImGui_ImplAndroid_HandleInputEvent(*ev);return r;}
void hookInput(){
 void*la=dlopen("libandroid.so",RTLD_NOW|RTLD_LOCAL);void*p=la?dlsym(la,"AInputQueue_getEvent"):nullptr;
 if(p&&DobbyHook(p,(void*)aqHook,(void**)&g_orig_aq_get)==0){AZLOGI("input=AInputQueue_getEvent");return;}
 const char*sym="_ZN7android13InputConsumer7consumeEPNS_26InputEventFactoryInterfaceEblPjPPNS_10InputEventE";
 const char*libs[]={"libinput.so","/system/lib64/libinput.so","/system/lib/libinput.so","/system_ext/lib64/libinput.so","/system_ext/lib/libinput.so",nullptr};
 for(int i=0;libs[i];++i){p=DobbySymbolResolver(libs[i],sym);if(p&&DobbyHook(p,(void*)consumeHook,(void**)&g_orig_consume)==0){AZLOGI("input=%s",libs[i]);return;}}
 AZLOGE("input hook unavailable");
}
void* hookWorker(void*){
 while(!g_activate.load())usleep(100000);
 void*egl=nullptr;for(int i=0;i<120&&!egl;i++){egl=dlopen("libEGL.so",RTLD_NOW|RTLD_LOCAL);if(!egl)usleep(250000);}
 if(!egl){AZLOGE("libEGL unavailable");return nullptr;}void*p=dlsym(egl,"eglSwapBuffers");if(!p){AZLOGE("eglSwapBuffers missing");return nullptr;}
 int rc=DobbyHook(p,(void*)swapHook,(void**)&g_orig_swap);AZLOGI("EGL hook rc=%d orig=%p",rc,(void*)g_orig_swap);if(rc==0&&g_orig_swap)hookInput();return nullptr;
}
}

extern "C" void az_ui_set_vm(JavaVM*vm){if(vm)g_vm=vm;}
extern "C" void az_ui_set_context(JNIEnv*env,jobject ctx){
 if(!env||!ctx)return;std::lock_guard<std::mutex>lk(g_jni_mu);if(g_context)env->DeleteGlobalRef(g_context);g_context=env->NewGlobalRef(ctx);
}
extern "C" void az_ui_activate(){
 g_activate=true;bool exp=false;if(g_hook_started.compare_exchange_strong(exp,true)){pthread_t t{};if(pthread_create(&t,nullptr,hookWorker,nullptr)==0)pthread_detach(t);}
}
