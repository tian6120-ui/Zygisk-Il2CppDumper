#include "game_ui.h"
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/keycodes.h>
#include <android/log.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
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
#include "imgui_internal.h"
#include "backends/imgui_impl_opengl3.h"

#define AZTAG "AZV7UI"
#define AZI(...) __android_log_print(ANDROID_LOG_INFO,AZTAG,__VA_ARGS__)
#define AZE(...) __android_log_print(ANDROID_LOG_ERROR,AZTAG,__VA_ARGS__)

namespace {
JavaVM* g_vm=nullptr;
jobject g_activity=nullptr;
jclass g_overlay_cls=nullptr;
jmethodID g_clip_get=nullptr,g_clip_set=nullptr;
std::mutex g_jni_mu;

ANativeWindow* g_window=nullptr;
std::mutex g_win_mu;
std::atomic<unsigned long long> g_surface_gen{0};
std::atomic<int> g_w{0},g_h{0};
std::atomic<bool> g_render_started{false};
std::atomic<bool> g_active{false};

struct Ev{int t,a,k;float x,y,v;bool c,s,al,me;std::string txt;};
std::mutex g_ev_mu;
std::vector<Ev> g_evs;

std::mutex g_region_mu;
float g_region[4]={18,90,125,200};

std::vector<char> g_script(256*1024,0);
std::string g_output,g_status="WAITING",g_base,g_last,g_req,g_out,g_stat,g_ui_cfg;
double g_poll=0;
bool g_open=false,g_autoscroll=true,g_center_next=false;
float g_font_scale=1.25f;
float g_font_base_px=20.0f;
int g_font_scale_idx=1;
const float kFontScales[]={1.00f,1.25f,1.50f,1.75f,2.00f,2.25f,2.50f};
const char* kFontScaleLabels[]={"100%","125%","150%","175%","200%","225%","250%"};

std::string pkg(){
 FILE*f=fopen("/proc/self/cmdline","rb");if(!f)return"unknown";char b[256]{};size_t n=fread(b,1,sizeof(b)-1,f);fclose(f);
 std::string s(b,n);auto z=s.find('\0');if(z!=std::string::npos)s.resize(z);auto c=s.find(':');if(c!=std::string::npos)s.resize(c);return s.empty()?"unknown":s;
}
void paths(){
 if(!g_base.empty())return;std::string files="/data/user/0/"+pkg()+"/files";g_base=files+"/AZTool";mkdir(files.c_str(),0700);mkdir(g_base.c_str(),0700);
 g_last=g_base+"/last.lua";g_req=g_base+"/script.req";g_out=g_base+"/script.out";g_stat=g_base+"/script.status";g_ui_cfg=g_base+"/ui.cfg";
}
std::string rf(const std::string&p,size_t lim=2*1024*1024){
 FILE*f=fopen(p.c_str(),"rb");if(!f)return{};std::string s;char b[4096];while(!feof(f)&&s.size()<lim){size_t n=fread(b,1,sizeof(b),f);if(!n)break;if(s.size()+n>lim)n=lim-s.size();s.append(b,n);}fclose(f);return s;
}
bool wf(const std::string&p,const std::string&s){FILE*f=fopen(p.c_str(),"wb");if(!f)return false;size_t n=fwrite(s.data(),1,s.size(),f);fclose(f);return n==s.size();}
double now(){timespec t{};clock_gettime(CLOCK_MONOTONIC,&t);return (double)t.tv_sec+t.tv_nsec/1e9;}
void poll(){
 double n=now();if(n-g_poll<.2)return;g_poll=n;paths();g_status=rf(g_stat,4096);while(!g_status.empty()&&(g_status.back()=='\n'||g_status.back()=='\r'))g_status.pop_back();g_output=rf(g_out);
}
void loadLast(){paths();auto s=rf(g_last,g_script.size()-1);memset(g_script.data(),0,g_script.size());if(!s.empty())memcpy(g_script.data(),s.data(),std::min(s.size(),g_script.size()-1));}
void loadUiConfig(){
 paths();auto s=rf(g_ui_cfg,4096);if(s.empty())return;
 auto p=s.find("fontScale=");if(p==std::string::npos)return;
 float v=strtof(s.c_str()+p+10,nullptr);
 int best=1;float d=1000.f;
 for(int i=0;i<7;++i){float nd=fabsf(kFontScales[i]-v);if(nd<d){d=nd;best=i;}}
 g_font_scale_idx=best;g_font_scale=kFontScales[best];
}
void saveUiConfig(){paths();char b[96];snprintf(b,sizeof(b),"fontScale=%.2f\n",g_font_scale);wf(g_ui_cfg,b);}
void applyFontScale(){
 if(!ImGui::GetCurrentContext())return;
 g_font_scale=kFontScales[std::clamp(g_font_scale_idx,0,6)];
 ImGui::GetIO().FontGlobalScale=g_font_scale;
}
void save(){paths();wf(g_last,std::string(g_script.data()));}
void run(){save();wf(g_req,"last.lua\n");}

JNIEnv* jenv(bool&detach){
 detach=false;if(!g_vm)return nullptr;JNIEnv*e=nullptr;if(g_vm->GetEnv((void**)&e,JNI_VERSION_1_6)==JNI_OK)return e;
 if(g_vm->AttachCurrentThread(&e,nullptr)==JNI_OK){detach=true;return e;}return nullptr;
}
std::string clipGet(){
 bool d=false;JNIEnv*e=jenv(d);if(!e||!g_overlay_cls||!g_clip_get)return{};jstring js=(jstring)e->CallStaticObjectMethod(g_overlay_cls,g_clip_get);
 if(e->ExceptionCheck()){e->ExceptionClear();js=nullptr;}std::string s;if(js){const char*p=e->GetStringUTFChars(js,nullptr);if(p){s=p;e->ReleaseStringUTFChars(js,p);}e->DeleteLocalRef(js);}if(d)g_vm->DetachCurrentThread();return s;
}
void clipSet(const std::string&s){
 bool d=false;JNIEnv*e=jenv(d);if(!e||!g_overlay_cls||!g_clip_set)return;jstring js=e->NewStringUTF(s.c_str());e->CallStaticVoidMethod(g_overlay_cls,g_clip_set,js);
 if(e->ExceptionCheck())e->ExceptionClear();e->DeleteLocalRef(js);if(d)g_vm->DetachCurrentThread();
}
void push(Ev e){std::lock_guard<std::mutex>lk(g_ev_mu);g_evs.emplace_back(std::move(e));}
void input(){
 std::vector<Ev> q;{std::lock_guard<std::mutex>lk(g_ev_mu);q.swap(g_evs);}ImGuiIO&io=ImGui::GetIO();
 for(auto&e:q){
  if(e.t==0){io.AddMousePosEvent(e.x,e.y);if(e.a==0)io.AddMouseButtonEvent(0,true);else if(e.a==1||e.a==3)io.AddMouseButtonEvent(0,false);}
  else if(e.t==1)io.AddMouseWheelEvent(0,e.v);
  else if(e.t==2)io.AddInputCharacter((unsigned)e.k);
  else if(e.t==3)io.AddInputCharactersUTF8(e.txt.c_str());
  else if(e.t==4){
   io.AddKeyEvent(ImGuiMod_Ctrl,e.c);io.AddKeyEvent(ImGuiMod_Shift,e.s);io.AddKeyEvent(ImGuiMod_Alt,e.al);io.AddKeyEvent(ImGuiMod_Super,e.me);
   ImGuiKey k=ImGuiKey_None;
   if(e.k==AKEYCODE_DEL)k=ImGuiKey_Backspace;else if(e.k==AKEYCODE_ENTER)k=ImGuiKey_Enter;else if(e.k==AKEYCODE_DPAD_LEFT)k=ImGuiKey_LeftArrow;else if(e.k==AKEYCODE_DPAD_RIGHT)k=ImGuiKey_RightArrow;
   else if(e.k==AKEYCODE_DPAD_UP)k=ImGuiKey_UpArrow;else if(e.k==AKEYCODE_DPAD_DOWN)k=ImGuiKey_DownArrow;else if(e.k==AKEYCODE_A)k=ImGuiKey_A;else if(e.k==AKEYCODE_C)k=ImGuiKey_C;else if(e.k==AKEYCODE_V)k=ImGuiKey_V;else if(e.k==AKEYCODE_X)k=ImGuiKey_X;
   if(k!=ImGuiKey_None)io.AddKeyEvent(k,e.a!=0);
  }
 }
}
void style(){
 ImGuiStyle&s=ImGui::GetStyle();s.WindowRounding=10;s.FrameRounding=7;s.ChildRounding=7;s.ScrollbarRounding=7;s.ScaleAllSizes(1.3f);
 auto*c=s.Colors;c[ImGuiCol_WindowBg]=ImVec4(.045f,.05f,.06f,.96f);c[ImGuiCol_TitleBgActive]=ImVec4(.48f,.02f,.05f,1);c[ImGuiCol_Button]=ImVec4(.34f,.035f,.065f,1);c[ImGuiCol_ButtonHovered]=ImVec4(.60f,.045f,.09f,1);
}
void region(float x1,float y1,float x2,float y2){std::lock_guard<std::mutex>lk(g_region_mu);g_region[0]=x1;g_region[1]=y1;g_region[2]=x2;g_region[3]=y2;}
void draw(int w,int h){
 poll();float x1=18,y1=90,x2=125,y2=200;
 ImGui::SetNextWindowPos(ImVec2(22,100),ImGuiCond_Once);ImGui::SetNextWindowSize(ImVec2(96,96),ImGuiCond_Always);
 if(ImGui::Begin("##AZ",nullptr,ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoSavedSettings)){
   if(ImGui::Button("AZ",ImVec2(78,78))){bool next=!g_open;g_open=next;if(next)g_center_next=true;}
   x1=std::min(x1,ImGui::GetWindowPos().x);y1=std::min(y1,ImGui::GetWindowPos().y);x2=std::max(x2,ImGui::GetWindowPos().x+ImGui::GetWindowSize().x);y2=std::max(y2,ImGui::GetWindowPos().y+ImGui::GetWindowSize().y);
 }ImGui::End();
 if(g_open){
  float pw=std::min((float)w-70.0f,std::max(680.0f,w*.66f));
  float ph=std::min((float)h-70.0f,std::max(540.0f,h*.76f));
  if(g_center_next){ImGui::SetNextWindowPos(ImVec2(w*.5f,h*.5f),ImGuiCond_Always,ImVec2(.5f,.5f));g_center_next=false;}
  ImGui::SetNextWindowSize(ImVec2(pw,ph),ImGuiCond_Once);
  if(ImGui::Begin("AZ ScriptCore V0.7",&g_open,ImGuiWindowFlags_NoSavedSettings)){
   auto p=ImGui::GetWindowPos(),s=ImGui::GetWindowSize();x1=std::min(x1,p.x);y1=std::min(y1,p.y);x2=std::max(x2,p.x+s.x);y2=std::max(y2,p.y+s.y);
   ImGui::Text("Status: %s",g_status.empty()?"WAITING":g_status.c_str());ImGui::SameLine();ImGui::TextDisabled("| %s",pkg().c_str());
   if(ImGui::BeginTabBar("tabs")){
    if(ImGui::BeginTabItem("Script")){
     if(ImGui::Button("Paste & Run")){auto s=clipGet();if(!s.empty()){memset(g_script.data(),0,g_script.size());memcpy(g_script.data(),s.data(),std::min(s.size(),g_script.size()-1));run();}}
     ImGui::SameLine();if(ImGui::Button("Run"))run();ImGui::SameLine();if(ImGui::Button("Save"))save();ImGui::SameLine();if(ImGui::Button("Load Last"))loadLast();ImGui::SameLine();if(ImGui::Button("Copy"))clipSet(std::string(g_script.data()));
     ImGui::Separator();ImGui::InputTextMultiline("##lua",g_script.data(),g_script.size(),ImVec2(-1,-1),ImGuiInputTextFlags_AllowTabInput);ImGui::EndTabItem();
    }
    if(ImGui::BeginTabItem("Output")){
     if(ImGui::Button("Copy All"))clipSet(g_output);ImGui::SameLine();if(ImGui::Button("Clear")){wf(g_out,"");g_output.clear();}ImGui::SameLine();ImGui::Checkbox("Auto-scroll",&g_autoscroll);
     ImGui::Separator();ImGui::BeginChild("out",ImVec2(0,0),true,ImGuiWindowFlags_HorizontalScrollbar);ImGui::TextUnformatted(g_output.c_str(),g_output.c_str()+g_output.size());if(g_autoscroll)ImGui::SetScrollHereY(1);ImGui::EndChild();ImGui::EndTabItem();
    }
    if(ImGui::BeginTabItem("Info")){
      ImGui::Text("V7 Java Overlay: CONNECTED");ImGui::Text("Surface: %d x %d",w,h);
      ImGui::Separator();
      ImGui::Text("Display");
      ImGui::SetNextItemWidth(180.0f);
      if(ImGui::BeginCombo("##scale_dropdown",kFontScaleLabels[g_font_scale_idx])){
        for(int i=0;i<7;++i){
          bool sel=(i==g_font_scale_idx);
          if(ImGui::Selectable(kFontScaleLabels[i],sel)){g_font_scale_idx=i;applyFontScale();saveUiConfig();}
          if(sel)ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
      }
      ImGui::SameLine();ImGui::TextDisabled("Font scale");
      ImGui::Text("Base font: %.0f px | Effective: %.0f px",g_font_base_px,g_font_base_px*g_font_scale);
      ImGui::TextWrapped("Copy Lua -> Paste & Run -> Output -> Copy All");
      ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
   }
  }ImGui::End();
 }
 region(std::max(0.f,x1-15),std::max(0.f,y1-15),std::min((float)w,x2+15),std::min((float)h,y2+15));
}

struct GL{EGLDisplay d=EGL_NO_DISPLAY;EGLContext c=EGL_NO_CONTEXT;EGLSurface s=EGL_NO_SURFACE;EGLConfig cfg{};ANativeWindow*w=nullptr;bool imgui=false;};
void dropSurface(GL&g){if(g.d!=EGL_NO_DISPLAY){eglMakeCurrent(g.d,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);if(g.s!=EGL_NO_SURFACE)eglDestroySurface(g.d,g.s);}g.s=EGL_NO_SURFACE;if(g.w){ANativeWindow_release(g.w);g.w=nullptr;}}
bool setup(GL&g,ANativeWindow*w){
 if(!w)return false;if(g.d==EGL_NO_DISPLAY){g.d=eglGetDisplay(EGL_DEFAULT_DISPLAY);if(g.d==EGL_NO_DISPLAY||!eglInitialize(g.d,nullptr,nullptr))return false;
  EGLint a[]={EGL_SURFACE_TYPE,EGL_WINDOW_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES3_BIT,EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_NONE},n=0;if(!eglChooseConfig(g.d,a,&g.cfg,1,&n)||n<1)return false;
  EGLint ca[]={EGL_CONTEXT_CLIENT_VERSION,3,EGL_NONE};g.c=eglCreateContext(g.d,g.cfg,EGL_NO_CONTEXT,ca);if(g.c==EGL_NO_CONTEXT)return false;}
 g.w=w;ANativeWindow_acquire(g.w);g.s=eglCreateWindowSurface(g.d,g.cfg,g.w,nullptr);if(g.s==EGL_NO_SURFACE){ANativeWindow_release(g.w);g.w=nullptr;return false;}if(!eglMakeCurrent(g.d,g.s,g.s,g.c)){dropSurface(g);return false;}
 eglSwapInterval(g.d,1);if(!g.imgui){
   IMGUI_CHECKVERSION();ImGui::CreateContext();
   ImGuiIO&io=ImGui::GetIO();io.IniFilename=nullptr;
   int sw=ANativeWindow_getWidth(w),sh=ANativeWindow_getHeight(w);int shortSide=std::max(1,std::min(sw,sh));
   g_font_base_px=std::clamp(shortSide/45.0f,18.0f,30.0f);
   ImFontConfig fc;fc.SizePixels=g_font_base_px;fc.OversampleH=2;fc.OversampleV=2;fc.PixelSnapH=false;
   io.Fonts->Clear();io.FontDefault=io.Fonts->AddFontDefault(&fc);
   loadUiConfig();applyFontScale();
   style();ImGui_ImplOpenGL3_Init("#version 300 es");loadLast();g.imgui=true;
   AZI("Font manager ready base=%.1f scale=%.2f",g_font_base_px,g_font_scale);
 }return true;
}
void* render(void*){
 GL gl;unsigned long long active=~0ULL;timespec last{};clock_gettime(CLOCK_MONOTONIC,&last);
 for(;;){
  auto gen=g_surface_gen.load();if(gen!=active){dropSurface(gl);ANativeWindow*w=nullptr;{std::lock_guard<std::mutex>lk(g_win_mu);if(g_window){ANativeWindow_acquire(g_window);w=g_window;}}if(w){setup(gl,w);ANativeWindow_release(w);}active=gen;}
  if(gl.s==EGL_NO_SURFACE||!gl.imgui){usleep(16000);continue;}int w=g_w.load(),h=g_h.load();if(w<=0||h<=0){usleep(16000);continue;}
  timespec t{};clock_gettime(CLOCK_MONOTONIC,&t);double dt=(t.tv_sec-last.tv_sec)+(t.tv_nsec-last.tv_nsec)/1e9;if(dt<=0||dt>.25)dt=1.0/60.0;last=t;
  ImGuiIO&io=ImGui::GetIO();io.DisplaySize=ImVec2((float)w,(float)h);io.DeltaTime=(float)dt;input();ImGui_ImplOpenGL3_NewFrame();ImGui::NewFrame();draw(w,h);ImGui::Render();
  glViewport(0,0,w,h);glDisable(GL_DEPTH_TEST);glEnable(GL_BLEND);glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);glClearColor(0,0,0,0);glClear(GL_COLOR_BUFFER_BIT);ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());eglSwapBuffers(gl.d,gl.s);
 }
 return nullptr;
}
void startRender(){bool x=false;if(g_render_started.compare_exchange_strong(x,true)){pthread_t t{};if(pthread_create(&t,nullptr,render,nullptr)==0)pthread_detach(t);}}

void JNICALL surfCreated(JNIEnv*e,jclass,jobject surface){ANativeWindow*w=ANativeWindow_fromSurface(e,surface);if(!w)return;{std::lock_guard<std::mutex>lk(g_win_mu);if(g_window)ANativeWindow_release(g_window);g_window=w;}g_w=ANativeWindow_getWidth(w);g_h=ANativeWindow_getHeight(w);g_surface_gen++;startRender();AZI("V7 Surface connected %dx%d",g_w.load(),g_h.load());}
void JNICALL surfChanged(JNIEnv*,jclass,jint w,jint h){g_w=w;g_h=h;g_surface_gen++;}
void JNICALL surfDestroyed(JNIEnv*,jclass){{std::lock_guard<std::mutex>lk(g_win_mu);if(g_window){ANativeWindow_release(g_window);g_window=nullptr;}}g_surface_gen++;}
jboolean JNICALL touch(JNIEnv*,jclass,jint a,jfloat x,jfloat y){push({0,(int)a,0,x,y,0,false,false,false,false,{}});return JNI_TRUE;}
void JNICALL scroll(JNIEnv*,jclass,jfloat v){push({1,0,0,0,0,v,false,false,false,false,{}});}
void JNICALL longPress(JNIEnv*,jclass,jfloat x,jfloat y){push({0,0,0,x,y,0,false,false,false,false,{}});}
void JNICALL touchRegion(JNIEnv*e,jclass,jfloatArray a){if(!a||e->GetArrayLength(a)<4)return;float r[4];{std::lock_guard<std::mutex>lk(g_region_mu);memcpy(r,g_region,sizeof(r));}e->SetFloatArrayRegion(a,0,4,r);}
void JNICALL theme(JNIEnv*e,jclass,jintArray a){if(!a)return;jsize n=e->GetArrayLength(a);std::vector<jint>v((size_t)n,0xff15171b);if(n>0)v[0]=0xff15171b;if(n>1)v[1]=0xff8a1020;if(n>2)v[2]=0xffffffff;if(n>3)v[3]=0xffb6b8bd;e->SetIntArrayRegion(a,0,n,v.data());}
void JNICALL addChar(JNIEnv*,jclass,jint c){push({2,0,(int)c,0,0,0,false,false,false,false,{}});}
void JNICALL commit(JNIEnv*e,jclass,jstring js){if(!js)return;const char*p=e->GetStringUTFChars(js,nullptr);std::string s=p?p:"";if(p)e->ReleaseStringUTFChars(js,p);Ev v{};v.t=3;v.txt=std::move(s);push(std::move(v));}
void JNICALL clearID(JNIEnv*,jclass){if(ImGui::GetCurrentContext())ImGui::ClearActiveID();}
void JNICALL key(JNIEnv*,jclass,jint k,jboolean down,jboolean ctrl,jboolean shift,jboolean alt,jboolean meta){Ev v{};v.t=4;v.k=k;v.a=down?1:0;v.c=ctrl;v.s=shift;v.al=alt;v.me=meta;push(std::move(v));}

bool registerV7(JNIEnv*e){
 jclass oc=e->FindClass("com/mxp/OverlaySurface");if(!oc){if(e->ExceptionCheck())e->ExceptionClear();AZE("OverlaySurface class unavailable");return false;}
 JNINativeMethod om[]={{(char*)"nativeSurfaceCreated",(char*)"(Landroid/view/Surface;)V",(void*)surfCreated},{(char*)"nativeSurfaceChanged",(char*)"(II)V",(void*)surfChanged},{(char*)"nativeSurfaceDestroyed",(char*)"()V",(void*)surfDestroyed},{(char*)"nativeTouch",(char*)"(IFF)Z",(void*)touch},{(char*)"nativeGetTouchRegion",(char*)"([F)V",(void*)touchRegion},{(char*)"nativeScroll",(char*)"(F)V",(void*)scroll},{(char*)"nativeLongPress",(char*)"(FF)V",(void*)longPress},{(char*)"nativeGetThemeColors",(char*)"([I)V",(void*)theme}};
 if(e->RegisterNatives(oc,om,sizeof(om)/sizeof(om[0]))!=0){if(e->ExceptionCheck())e->ExceptionClear();e->DeleteLocalRef(oc);AZE("OverlaySurface RegisterNatives failed");return false;}
 {std::lock_guard<std::mutex>lk(g_jni_mu);if(g_overlay_cls)e->DeleteGlobalRef(g_overlay_cls);g_overlay_cls=(jclass)e->NewGlobalRef(oc);g_clip_get=e->GetStaticMethodID(oc,"clipboardGet","()Ljava/lang/String;");g_clip_set=e->GetStaticMethodID(oc,"clipboardSet","(Ljava/lang/String;)V");if(e->ExceptionCheck())e->ExceptionClear();}
 e->DeleteLocalRef(oc);
 jclass hc=e->FindClass("com/mxp/Helper");if(hc){JNINativeMethod hm[]={{(char*)"nativeAddChar",(char*)"(I)V",(void*)addChar},{(char*)"nativeCommitText",(char*)"(Ljava/lang/String;)V",(void*)commit},{(char*)"nativeClearActiveID",(char*)"()V",(void*)clearID},{(char*)"nativeKeyEvent",(char*)"(IZZZZ)V",(void*)key}};e->RegisterNatives(hc,hm,sizeof(hm)/sizeof(hm[0]));if(e->ExceptionCheck())e->ExceptionClear();e->DeleteLocalRef(hc);}
 AZI("V7 Java Overlay natives registered");return true;
}
}

extern "C" void az_ui_set_vm(JavaVM*vm){if(vm)g_vm=vm;}
extern "C" void az_ui_set_context(JNIEnv*e,jobject activity){
 if(!e||!activity)return;
 {
   std::lock_guard<std::mutex>lk(g_jni_mu);
   if(g_activity)e->DeleteGlobalRef(g_activity);
   g_activity=e->NewGlobalRef(activity);
 }
 if(!registerV7(e)){
   AZE("V7 Overlay registration failed");
   return;
 }
 jclass oc=e->FindClass("com/mxp/OverlaySurface");
 if(!oc){
   if(e->ExceptionCheck())e->ExceptionClear();
   AZE("OverlaySurface.show: class unavailable");
   return;
 }
 jmethodID show=e->GetStaticMethodID(oc,"show","(Landroid/app/Activity;)V");
 if(!show){
   if(e->ExceptionCheck())e->ExceptionClear();
   AZE("OverlaySurface.show: method unavailable");
   e->DeleteLocalRef(oc);
   return;
 }
 e->CallStaticVoidMethod(oc,show,activity);
 if(e->ExceptionCheck()){
   e->ExceptionClear();
   AZE("OverlaySurface.show: Java exception");
 }else{
   AZI("OverlaySurface.show(activity) invoked");
 }
 e->DeleteLocalRef(oc);
 startRender();
}
extern "C" void az_ui_activate(){g_active=true;startRender();}
