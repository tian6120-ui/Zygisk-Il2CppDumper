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
#include <dirent.h>
#include <cmath>
#include <cstdlib>
#include "imgui.h"
#include "imgui_internal.h"
#include "backends/imgui_impl_opengl3.h"
#include "zh_glyphs.h"

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
std::string g_output,g_status="WAITING",g_base,g_last,g_req,g_out,g_stat,g_ui_cfg,g_scripts_dir;
double g_poll=0;
bool g_open=false,g_autoscroll=true,g_center_next=false;
float g_font_scale=1.25f;
float g_font_base_px=20.0f;
int g_font_scale_idx=1;
int g_language=1;
int g_page=0;
float g_accent[3]={0.58f,0.04f,0.08f};
float g_bg[3]={0.045f,0.05f,0.06f};
float g_text[3]={0.94f,0.94f,0.95f};
float g_panel_alpha=0.94f;
char g_custom_dir[512]="/sdcard/Download";
const float kFontScales[]={1.00f,1.25f,1.50f,1.75f,2.00f,2.25f,2.50f};
const char* kFontScaleLabels[]={"100%","125%","150%","175%","200%","225%","250%"};
struct LocalScript{std::string name,path;};
std::vector<LocalScript> g_local_scripts;
int g_local_selected=-1;
std::string g_local_note;

std::string pkg(){
 FILE*f=fopen("/proc/self/cmdline","rb");if(!f)return"unknown";char b[256]{};size_t n=fread(b,1,sizeof(b)-1,f);fclose(f);
 std::string s(b,n);auto z=s.find('\0');if(z!=std::string::npos)s.resize(z);auto c=s.find(':');if(c!=std::string::npos)s.resize(c);return s.empty()?"unknown":s;
}
void paths(){
 if(!g_base.empty())return;std::string files="/data/user/0/"+pkg()+"/files";g_base=files+"/AZTool";mkdir(files.c_str(),0700);mkdir(g_base.c_str(),0700);
 g_last=g_base+"/last.lua";g_req=g_base+"/script.req";g_out=g_base+"/script.out";g_stat=g_base+"/script.status";g_ui_cfg=g_base+"/ui.cfg";g_scripts_dir=g_base+"/scripts";mkdir(g_scripts_dir.c_str(),0700);
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
std::string cfgValue(const std::string&s,const char*key){
 auto p=s.find(key);if(p==std::string::npos)return{};p+=strlen(key);auto e=s.find('\n',p);return s.substr(p,e==std::string::npos?s.size()-p:e-p);
}
void parse3(const std::string&v,float out[3]){
 float a,b,c;if(sscanf(v.c_str(),"%f,%f,%f",&a,&b,&c)==3){out[0]=std::clamp(a,0.f,1.f);out[1]=std::clamp(b,0.f,1.f);out[2]=std::clamp(c,0.f,1.f);}
}
void loadUiConfig(){
 paths();auto s=rf(g_ui_cfg,4096);if(s.empty())return;
 auto fs=cfgValue(s,"fontScale=");if(!fs.empty()){
  float v=strtof(fs.c_str(),nullptr);int best=1;float d=1000.f;
  for(int i=0;i<7;++i){float nd=fabsf(kFontScales[i]-v);if(nd<d){d=nd;best=i;}}
  g_font_scale_idx=best;g_font_scale=kFontScales[best];
 }
 auto la=cfgValue(s,"language=");if(!la.empty())g_language=std::clamp(atoi(la.c_str()),0,1);
 auto al=cfgValue(s,"alpha=");if(!al.empty())g_panel_alpha=std::clamp(strtof(al.c_str(),nullptr),0.25f,1.0f);
 auto ac=cfgValue(s,"accent=");if(!ac.empty())parse3(ac,g_accent);
 auto bg=cfgValue(s,"bg=");if(!bg.empty())parse3(bg,g_bg);
 auto tx=cfgValue(s,"text=");if(!tx.empty())parse3(tx,g_text);
 auto cd=cfgValue(s,"customDir=");if(!cd.empty()){strncpy(g_custom_dir,cd.c_str(),sizeof(g_custom_dir)-1);g_custom_dir[sizeof(g_custom_dir)-1]=0;}
}
void saveUiConfig(){
 paths();char b[1200];
 snprintf(b,sizeof(b),
  "fontScale=%.2f\nlanguage=%d\nalpha=%.3f\naccent=%.4f,%.4f,%.4f\nbg=%.4f,%.4f,%.4f\ntext=%.4f,%.4f,%.4f\ncustomDir=%s\n",
  g_font_scale,g_language,g_panel_alpha,g_accent[0],g_accent[1],g_accent[2],g_bg[0],g_bg[1],g_bg[2],g_text[0],g_text[1],g_text[2],g_custom_dir);
 wf(g_ui_cfg,b);
}
void applyFontScale(){
 if(!ImGui::GetCurrentContext())return;
 g_font_scale=kFontScales[std::clamp(g_font_scale_idx,0,6)];
 ImGui::GetIO().FontGlobalScale=g_font_scale;
}
const char* L(const char*en,const char*zh){return g_language?zh:en;}

int b64v(char c){
 if(c>='A'&&c<='Z')return c-'A';if(c>='a'&&c<='z')return c-'a'+26;if(c>='0'&&c<='9')return c-'0'+52;if(c=='+')return 62;if(c=='/')return 63;return -1;
}
std::vector<unsigned char> decodeB64(const char*s){
 std::vector<unsigned char>o;o.reserve(AZ_ZH_PACKED_BYTES);int val=0,bits=-8;
 for(;*s;++s){int d=b64v(*s);if(d<0)continue;val=(val<<6)|d;bits+=6;if(bits>=0){o.push_back((unsigned char)((val>>bits)&0xff));bits-=8;}}
 return o;
}
void installZhGlyphs(ImFont*font){
 if(!font)return;ImGuiIO&io=ImGui::GetIO();std::vector<int> rects;rects.reserve(AZ_ZH_GLYPH_COUNT);
 for(int i=0;i<AZ_ZH_GLYPH_COUNT;++i)rects.push_back(io.Fonts->AddCustomRectFontGlyph(font,(ImWchar)AZ_ZH_GLYPHS[i].code,AZ_ZH_W,AZ_ZH_H,(float)AZ_ZH_W,ImVec2(0,-1)));
 unsigned char*pixels=nullptr;int tw=0,th=0;io.Fonts->GetTexDataAsAlpha8(&pixels,&tw,&th);auto packed=decodeB64(AZ_ZH_DATA_B64);
 if(!pixels||packed.size()<AZ_ZH_PACKED_BYTES)return;
 for(int i=0;i<AZ_ZH_GLYPH_COUNT;++i){
  auto*r=io.Fonts->GetCustomRectByIndex(rects[i]);const auto&m=AZ_ZH_GLYPHS[i];
  for(int p=0;p<AZ_ZH_W*AZ_ZH_H;++p){unsigned char b=packed[m.offset+(p>>2)];int shift=6-2*(p&3);unsigned char a=(unsigned char)(((b>>shift)&3)*85);int x=p%AZ_ZH_W,y=p/AZ_ZH_W;unsigned char&dst=pixels[(r->Y+y)*tw+r->X+x];if(a>dst)dst=a;}
 }
 AZI("Built-in Chinese UI glyphs installed count=%d",AZ_ZH_GLYPH_COUNT);
}

bool luaName(const char*n){if(!n)return false;size_t l=strlen(n);return l>4&&strcasecmp(n+l-4,".lua")==0;}
void addScriptPath(const std::string&p,const std::string&name){
 for(auto&s:g_local_scripts)if(s.path==p)return;
 if(g_local_scripts.size()<400)g_local_scripts.push_back({name,p});
}
void scanDir(const std::string&dir,int depth){
 DIR*d=opendir(dir.c_str());if(!d)return;dirent*e;
 while((e=readdir(d))){if(!strcmp(e->d_name,".")||!strcmp(e->d_name,".."))continue;std::string p=dir+"/"+e->d_name;struct stat st{};if(stat(p.c_str(),&st)!=0)continue;
  if(S_ISREG(st.st_mode)&&luaName(e->d_name)&&access(p.c_str(),R_OK)==0)addScriptPath(p,e->d_name);
  else if(depth>0&&S_ISDIR(st.st_mode)&&e->d_name[0]!='.')scanDir(p,depth-1);
  if(g_local_scripts.size()>=400)break;
 }
 closedir(d);
}
void refreshLocalScripts(){
 paths();g_local_scripts.clear();g_local_selected=-1;g_local_note.clear();
 scanDir(g_scripts_dir,1);scanDir("/sdcard/Download/AZScript",1);scanDir("/sdcard/Download",0);scanDir("/storage/emulated/0/Download/AZScript",1);scanDir("/data/local/tmp/AZScript",1);
 if(g_custom_dir[0])scanDir(g_custom_dir,1);
 std::sort(g_local_scripts.begin(),g_local_scripts.end(),[](const LocalScript&a,const LocalScript&b){return a.name<b.name;});
 if(g_local_scripts.empty())g_local_note=L("No readable .lua files found.","未找到可读 Lua 文件");
 else {char b[96];snprintf(b,sizeof(b),g_language?"已找到 %zu 个脚本":"Found %zu scripts",g_local_scripts.size());g_local_note=b;}
}
std::string clipGet();
bool loadScriptPath(const std::string&p){
 auto s=rf(p,g_script.size());if(s.empty()){g_local_note=L("Read failed or file is empty.","读取失败或文件为空");return false;}
 memset(g_script.data(),0,g_script.size());memcpy(g_script.data(),s.data(),std::min(s.size(),g_script.size()-1));g_local_note=p;return true;
}
void save(){paths();wf(g_last,std::string(g_script.data()));}
void run(){save();wf(g_req,"last.lua\n");}
void addClipboardOnly(){
 auto v=clipGet();
 if(v.empty())return;
 size_t cur=strnlen(g_script.data(),g_script.size());
 size_t cap=g_script.size()-1;
 if(cur>=cap)return;
 if(cur>0&&g_script[cur-1]!='\n'&&cur<cap)g_script[cur++]='\n';
 size_t n=std::min(v.size(),cap-cur);
 memcpy(g_script.data()+cur,v.data(),n);
 g_script[cur+n]=0;
}

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
ImVec4 mix3(const float c[3],float mul,float a){return ImVec4(std::clamp(c[0]*mul,0.f,1.f),std::clamp(c[1]*mul,0.f,1.f),std::clamp(c[2]*mul,0.f,1.f),a);}
void applyTheme(){
 ImGuiStyle&s=ImGui::GetStyle();auto*c=s.Colors;
 c[ImGuiCol_Text]=ImVec4(g_text[0],g_text[1],g_text[2],1);
 c[ImGuiCol_TextDisabled]=mix3(g_text,.60f,1);
 c[ImGuiCol_WindowBg]=ImVec4(g_bg[0],g_bg[1],g_bg[2],g_panel_alpha);
 c[ImGuiCol_ChildBg]=ImVec4(g_bg[0],g_bg[1],g_bg[2],std::min(1.f,g_panel_alpha*.82f));
 c[ImGuiCol_PopupBg]=ImVec4(g_bg[0],g_bg[1],g_bg[2],std::min(1.f,g_panel_alpha+.04f));
 c[ImGuiCol_Border]=mix3(g_text,.28f,.55f);
 c[ImGuiCol_FrameBg]=mix3(g_bg,1.75f,.92f);c[ImGuiCol_FrameBgHovered]=mix3(g_bg,2.10f,.96f);c[ImGuiCol_FrameBgActive]=mix3(g_bg,2.35f,.98f);
 c[ImGuiCol_TitleBg]=mix3(g_accent,.58f,1);c[ImGuiCol_TitleBgActive]=mix3(g_accent,.90f,1);
 c[ImGuiCol_Button]=mix3(g_accent,.68f,1);c[ImGuiCol_ButtonHovered]=mix3(g_accent,1.00f,1);c[ImGuiCol_ButtonActive]=mix3(g_accent,1.15f,1);
 c[ImGuiCol_Header]=mix3(g_accent,.60f,.88f);c[ImGuiCol_HeaderHovered]=mix3(g_accent,.90f,.95f);c[ImGuiCol_HeaderActive]=mix3(g_accent,1.05f,1);
 c[ImGuiCol_CheckMark]=mix3(g_accent,1.35f,1);c[ImGuiCol_SliderGrab]=mix3(g_accent,1.15f,1);c[ImGuiCol_SliderGrabActive]=mix3(g_accent,1.35f,1);
 c[ImGuiCol_Tab]=mix3(g_bg,1.5f,1);c[ImGuiCol_TabHovered]=mix3(g_accent,.85f,1);c[ImGuiCol_TabActive]=mix3(g_accent,.62f,1);
}
void style(){
 ImGuiStyle&s=ImGui::GetStyle();
 s.WindowRounding=12;s.ChildRounding=9;s.FrameRounding=7;s.PopupRounding=8;s.ScrollbarRounding=8;s.TabRounding=8;
 s.WindowBorderSize=1.0f;s.ChildBorderSize=1.0f;s.FrameBorderSize=0.0f;
 s.WindowPadding=ImVec2(16,14);s.FramePadding=ImVec2(12,8);s.ItemSpacing=ImVec2(10,9);s.ItemInnerSpacing=ImVec2(8,6);
 s.ScrollbarSize=15.0f;s.ScaleAllSizes(1.15f);applyTheme();
}
void presetTheme(int id){
 if(id==0){g_accent[0]=.58f;g_accent[1]=.04f;g_accent[2]=.08f;g_bg[0]=.045f;g_bg[1]=.05f;g_bg[2]=.06f;}
 if(id==1){g_accent[0]=.10f;g_accent[1]=.35f;g_accent[2]=.88f;g_bg[0]=.035f;g_bg[1]=.045f;g_bg[2]=.07f;}
 if(id==2){g_accent[0]=.08f;g_accent[1]=.62f;g_accent[2]=.34f;g_bg[0]=.035f;g_bg[1]=.06f;g_bg[2]=.05f;}
 if(id==3){g_accent[0]=.55f;g_accent[1]=.20f;g_accent[2]=.78f;g_bg[0]=.055f;g_bg[1]=.04f;g_bg[2]=.07f;}
 applyTheme();saveUiConfig();
}
void sectionTitle(const char*title,const char*sub=nullptr){
 ImVec2 p=ImGui::GetCursorScreenPos();float h=sub?46.0f:30.0f;
 ImDrawList*dl=ImGui::GetWindowDrawList();
 dl->AddRectFilled(ImVec2(p.x,p.y),ImVec2(p.x+5,p.y+h),ImGui::ColorConvertFloat4ToU32(mix3(g_accent,1.15f,1)),3.0f);
 ImGui::SetCursorPosX(ImGui::GetCursorPosX()+14);
 ImGui::TextUnformatted(title);
 if(sub){ImGui::SetCursorPosX(ImGui::GetCursorPosX()+14);ImGui::TextDisabled("%s",sub);}
 ImGui::Dummy(ImVec2(0,4));
}
void navButton(const char*label,int page,int&state,float w=150.0f){
 bool sel=state==page;
 if(sel){ImGui::PushStyleColor(ImGuiCol_Button,mix3(g_accent,.82f,1));ImGui::PushStyleColor(ImGuiCol_ButtonHovered,mix3(g_accent,.98f,1));}
 else {ImGui::PushStyleColor(ImGuiCol_Button,mix3(g_bg,1.65f,.95f));ImGui::PushStyleColor(ImGuiCol_ButtonHovered,mix3(g_bg,2.10f,.98f));}
 if(ImGui::Button(label,ImVec2(w,42)))state=page;
 ImGui::PopStyleColor(2);
}
void region(float x1,float y1,float x2,float y2){std::lock_guard<std::mutex>lk(g_region_mu);g_region[0]=x1;g_region[1]=y1;g_region[2]=x2;g_region[3]=y2;}
void draw(int w,int h){
 poll();float x1=18,y1=90,x2=125,y2=200;
 ImGui::SetNextWindowPos(ImVec2(22,100),ImGuiCond_Once);ImGui::SetNextWindowSize(ImVec2(96,96),ImGuiCond_Always);
 if(ImGui::Begin("##AZ",nullptr,ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoSavedSettings)){
   ImGui::PushStyleColor(ImGuiCol_Button,mix3(g_accent,.82f,.96f));
   ImGui::PushStyleColor(ImGuiCol_ButtonHovered,mix3(g_accent,1.05f,1));
   if(ImGui::Button("AZ",ImVec2(78,78))){bool next=!g_open;g_open=next;if(next)g_center_next=true;}
   ImGui::PopStyleColor(2);
   x1=std::min(x1,ImGui::GetWindowPos().x);y1=std::min(y1,ImGui::GetWindowPos().y);x2=std::max(x2,ImGui::GetWindowPos().x+ImGui::GetWindowSize().x);y2=std::max(y2,ImGui::GetWindowPos().y+ImGui::GetWindowSize().y);
 }ImGui::End();

 if(g_open){
  float pw=std::min((float)w-70.0f,std::max(820.0f,w*.72f));
  float ph=std::min((float)h-54.0f,std::max(620.0f,h*.84f));
  if(g_center_next){ImGui::SetNextWindowPos(ImVec2(w*.5f,h*.5f),ImGuiCond_Always,ImVec2(.5f,.5f));g_center_next=false;}
  ImGui::SetNextWindowSize(ImVec2(pw,ph),ImGuiCond_Once);
  if(ImGui::Begin(g_language?"AZ ScriptCore · 中文":"AZ ScriptCore",&g_open,ImGuiWindowFlags_NoSavedSettings)){
   auto p=ImGui::GetWindowPos(),s=ImGui::GetWindowSize();x1=std::min(x1,p.x);y1=std::min(y1,p.y);x2=std::max(x2,p.x+s.x);y2=std::max(y2,p.y+s.y);

   ImGui::BeginChild("##topbar",ImVec2(0,64),true,ImGuiWindowFlags_NoScrollbar);
   ImGui::TextUnformatted("AZ ScriptCore");ImGui::SameLine();ImGui::TextDisabled("V7 Path");
   ImGui::TextDisabled("%s",pkg().c_str());
   const char*st=g_status.empty()?"WAITING":g_status.c_str();
   ImVec4 sc=(g_status.find("RUNNING")!=std::string::npos)?ImVec4(.95f,.72f,.20f,1):
             (g_status.find("DONE")!=std::string::npos)?ImVec4(.30f,.90f,.52f,1):
             (g_status.find("ERROR")!=std::string::npos)?ImVec4(.98f,.30f,.34f,1):ImVec4(.65f,.68f,.72f,1);
   ImGui::SameLine();ImGui::TextColored(sc,"  %s: %s",L("Status","状态"),st);
   ImGui::EndChild();

   ImGui::Dummy(ImVec2(0,4));
   ImGui::BeginChild("##nav",ImVec2(0,58),true,ImGuiWindowFlags_NoScrollbar);
   navButton(L("Script","脚本"),0,g_page,160);ImGui::SameLine();
   bool localSel=g_page==1;
   if(localSel){ImGui::PushStyleColor(ImGuiCol_Button,mix3(g_accent,.82f,1));ImGui::PushStyleColor(ImGuiCol_ButtonHovered,mix3(g_accent,.98f,1));}
   else {ImGui::PushStyleColor(ImGuiCol_Button,mix3(g_bg,1.65f,.95f));ImGui::PushStyleColor(ImGuiCol_ButtonHovered,mix3(g_bg,2.10f,.98f));}
   if(ImGui::Button(L("Local Scripts","本地脚本"),ImVec2(180,42))){g_page=1;if(g_local_scripts.empty())refreshLocalScripts();}
   ImGui::PopStyleColor(2);ImGui::SameLine();
   navButton(L("Theme","主题"),2,g_page,150);
   ImGui::EndChild();
   ImGui::Dummy(ImVec2(0,5));

   if(g_page==0){
    ImGui::BeginChild("##script_toolbar",ImVec2(0,64),true,ImGuiWindowFlags_NoScrollbar);
    if(ImGui::Button(L("Add","添加"),ImVec2(120,42)))addClipboardOnly();
    ImGui::SameLine();
    if(ImGui::Button(L("Paste & Run","粘贴并运行"),ImVec2(175,42))){auto v=clipGet();if(!v.empty()){memset(g_script.data(),0,g_script.size());memcpy(g_script.data(),v.data(),std::min(v.size(),g_script.size()-1));run();}}
    ImGui::SameLine();if(ImGui::Button(L("Run","运行"),ImVec2(110,42)))run();
    ImGui::SameLine();if(ImGui::Button(L("Save","保存"),ImVec2(110,42)))save();
    ImGui::SameLine();if(ImGui::Button(L("Clear","清空"),ImVec2(110,42))){memset(g_script.data(),0,g_script.size());}
    ImGui::EndChild();

    float avail=ImGui::GetContentRegionAvail().y;float editorH=std::max(220.f,avail*.51f);
    ImGui::BeginChild("##editor_card",ImVec2(0,editorH),true);
    char sub[96];snprintf(sub,sizeof(sub),g_language?"UTF-8 · 当前 %zu 字节":"UTF-8 · %zu bytes",strnlen(g_script.data(),g_script.size()));
    sectionTitle(L("Script input","脚本输入"),sub);
    ImGui::PushStyleColor(ImGuiCol_FrameBg,mix3(g_bg,1.15f,.98f));
    ImGui::InputTextMultiline("##lua",g_script.data(),g_script.size(),ImVec2(-1,-1),ImGuiInputTextFlags_AllowTabInput);
    ImGui::PopStyleColor();
    ImGui::EndChild();

    ImGui::Dummy(ImVec2(0,6));
    ImGui::BeginChild("##output_card",ImVec2(0,0),true);
    sectionTitle("OUT",L("Run result","运行结果"));
    if(ImGui::Button(L("Copy All","复制全部")))clipSet(g_output);ImGui::SameLine();
    if(ImGui::Button(L("Clear Output","清空输出"))){wf(g_out,"");g_output.clear();}ImGui::SameLine();
    ImGui::Checkbox(L("Auto-scroll","自动滚动"),&g_autoscroll);
    ImGui::Separator();
    ImGui::BeginChild("##out_text",ImVec2(0,0),false,ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::TextUnformatted(g_output.c_str(),g_output.c_str()+g_output.size());
    if(g_autoscroll)ImGui::SetScrollHereY(1);
    ImGui::EndChild();ImGui::EndChild();
   }else if(g_page==1){
    ImGui::BeginChild("##local_card",ImVec2(0,0),true);
    sectionTitle(L("Local Lua files","本地 Lua 脚本"),L("Choose a file, load it into the editor, or run it directly.","选择脚本后可加载到编辑器或直接运行"));
    ImGui::TextWrapped("%s: %s",L("Built-in folder","内置目录"),g_scripts_dir.c_str());
    ImGui::TextUnformatted(L("Custom folder","自定义目录"));ImGui::SameLine();ImGui::SetNextItemWidth(-170);ImGui::InputText("##customdir",g_custom_dir,sizeof(g_custom_dir));ImGui::SameLine();
    if(ImGui::Button(L("Refresh","刷新"))){saveUiConfig();refreshLocalScripts();}
    ImGui::TextDisabled("%s",g_local_note.c_str());ImGui::Separator();
    float listH=std::max(280.f,ImGui::GetContentRegionAvail().y-105.f);
    ImGui::BeginChild("##local_list",ImVec2(0,listH),true);
    for(size_t i=0;i<g_local_scripts.size();++i){
      bool sel=(int)i==g_local_selected;std::string label=g_local_scripts[i].name+"##"+std::to_string(i);
      if(ImGui::Selectable(label.c_str(),sel)){g_local_selected=(int)i;g_local_note=g_local_scripts[i].path;}
    }
    ImGui::EndChild();
    if(g_local_selected>=0&&g_local_selected<(int)g_local_scripts.size()){
      const auto&ls=g_local_scripts[g_local_selected];ImGui::TextWrapped("%s: %s",L("Selected","已选择"),ls.path.c_str());
      if(ImGui::Button(L("Load to editor","加载到编辑器"),ImVec2(180,42))){if(loadScriptPath(ls.path))g_page=0;}
      ImGui::SameLine();if(ImGui::Button(L("Run directly","直接运行"),ImVec2(160,42))){if(loadScriptPath(ls.path)){run();g_page=0;}}
    }
    ImGui::EndChild();
   }else{
    ImGui::BeginChild("##theme_card",ImVec2(0,0),true);
    sectionTitle(L("Appearance","外观设置"),L("Colors and opacity are saved automatically.","颜色与透明度自动保存"));
    ImGui::BeginChild("##theme_general",ImVec2(0,145),true);
    ImGui::TextUnformatted(L("Language","界面语言"));ImGui::SameLine();ImGui::SetNextItemWidth(190);
    if(ImGui::BeginCombo("##lang",g_language?"中文":"English")){
      if(ImGui::Selectable("English",g_language==0)){g_language=0;saveUiConfig();}
      if(ImGui::Selectable("中文",g_language==1)){g_language=1;saveUiConfig();}
      ImGui::EndCombo();
    }
    ImGui::TextUnformatted(L("Font size","字体大小"));ImGui::SameLine();ImGui::SetNextItemWidth(190);
    if(ImGui::BeginCombo("##scale",kFontScaleLabels[g_font_scale_idx])){for(int i=0;i<7;++i){bool sel=i==g_font_scale_idx;if(ImGui::Selectable(kFontScaleLabels[i],sel)){g_font_scale_idx=i;applyFontScale();saveUiConfig();}if(sel)ImGui::SetItemDefaultFocus();}ImGui::EndCombo();}
    ImGui::EndChild();

    ImGui::Dummy(ImVec2(0,6));sectionTitle(L("Quick themes","快捷主题"));
    if(ImGui::Button(L("Dark Red","深红"),ImVec2(130,42)))presetTheme(0);ImGui::SameLine();
    if(ImGui::Button(L("Blue","蓝色"),ImVec2(130,42)))presetTheme(1);ImGui::SameLine();
    if(ImGui::Button(L("Green","绿色"),ImVec2(130,42)))presetTheme(2);ImGui::SameLine();
    if(ImGui::Button(L("Purple","紫色"),ImVec2(130,42)))presetTheme(3);

    ImGui::Dummy(ImVec2(0,6));sectionTitle(L("Custom colors","自定义颜色"));
    if(ImGui::ColorEdit3(L("Accent","强调色"),g_accent)){applyTheme();saveUiConfig();}
    if(ImGui::ColorEdit3(L("Background","背景色"),g_bg)){applyTheme();saveUiConfig();}
    if(ImGui::ColorEdit3(L("Text","文字色"),g_text)){applyTheme();saveUiConfig();}
    if(ImGui::SliderFloat(L("Window opacity","窗口透明度"),&g_panel_alpha,.25f,1.0f,"%.2f")){applyTheme();saveUiConfig();}
    ImGui::Dummy(ImVec2(0,6));
    if(ImGui::Button(L("Restore default","恢复默认"),ImVec2(180,42))){g_accent[0]=.58f;g_accent[1]=.04f;g_accent[2]=.08f;g_bg[0]=.045f;g_bg[1]=.05f;g_bg[2]=.06f;g_text[0]=g_text[1]=g_text[2]=.94f;g_panel_alpha=.94f;applyTheme();saveUiConfig();}
    ImGui::EndChild();
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
   installZhGlyphs(io.FontDefault);
   loadUiConfig();applyFontScale();
   style();ImGui_ImplOpenGL3_Init("#version 300 es");loadLast();refreshLocalScripts();g.imgui=true;
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
jint argb(const float c[3],float a=1.f){int A=(int)(std::clamp(a,0.f,1.f)*255+.5f),R=(int)(std::clamp(c[0],0.f,1.f)*255+.5f),G=(int)(std::clamp(c[1],0.f,1.f)*255+.5f),B=(int)(std::clamp(c[2],0.f,1.f)*255+.5f);return (jint)((A<<24)|(R<<16)|(G<<8)|B);}
void JNICALL theme(JNIEnv*e,jclass,jintArray a){if(!a)return;jsize n=e->GetArrayLength(a);std::vector<jint>v((size_t)n,argb(g_bg,g_panel_alpha));if(n>0)v[0]=argb(g_bg,g_panel_alpha);if(n>1)v[1]=argb(g_accent,1);if(n>2)v[2]=argb(g_text,1);if(n>3)v[3]=argb(g_text,.72f);e->SetIntArrayRegion(a,0,n,v.data());}
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
