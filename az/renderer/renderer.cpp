#include <android/input.h>
#include <android/log.h>
#include <dlfcn.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>

#include "imgui.h"
#include "backends/imgui_impl_opengl3.h"
#include "backends/imgui_impl_android.h"
#include "dobby.h"

#define TAG "AZR06"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

static void flog(const char* s) {
    FILE* f = fopen("/data/local/tmp/AZTool/renderer.log", "a");
    if (f) {
        fprintf(f, "%s\n", s);
        fclose(f);
    }
    LOGI("%s", s);
}

static void flogf(const char* fmt, ...) {
    char b[768];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    flog(b);
}

static EGLBoolean (*g_origSwap)(EGLDisplay,EGLSurface) = nullptr;
static bool g_imguiReady = false;
static bool g_panelOpen = true;
static int g_w = 0;
static int g_h = 0;
static timespec g_lastTs{};

using ConsumeFn = int32_t(*)(void*, void*, bool, int64_t, uint32_t*, AInputEvent**);
static ConsumeFn g_origConsume = nullptr;

static int32_t hookConsume(void* thiz, void* arg1, bool arg2, int64_t arg3, uint32_t* arg4, AInputEvent** ev) {
    int32_t r = g_origConsume ? g_origConsume(thiz,arg1,arg2,arg3,arg4,ev) : -1;
    if (r == 0 && ev && *ev && ImGui::GetCurrentContext()) {
        ImGui_ImplAndroid_HandleInputEvent(*ev);
    }
    return r;
}

static bool hookInput() {
    const char* images[] = {
        "libinput.so",
        "/system/lib64/libinput.so",
        "/system_ext/lib64/libinput.so",
        nullptr
    };
    const char* sym = "_ZN7android13InputConsumer7consumeEPNS_26InputEventFactoryInterfaceEblPjPPNS_10InputEventE";
    for (int i=0; images[i]; ++i) {
        void* p = DobbySymbolResolver(images[i], sym);
        if (!p) continue;
        int rc = DobbyHook(p, (void*)hookConsume, (void**)&g_origConsume);
        flogf("[AZR06] input hook image=%s rc=%d target=%p orig=%p", images[i], rc, p, (void*)g_origConsume);
        if (rc == 0 && g_origConsume) return true;
    }
    flog("[AZR06] input hook unavailable; render path remains active");
    return false;
}

static void setupStyle() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 10.0f;
    s.ChildRounding = 8.0f;
    s.FrameRounding = 7.0f;
    s.PopupRounding = 8.0f;
    s.ScrollbarRounding = 7.0f;
    s.GrabRounding = 7.0f;

    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg]      = ImVec4(0.045f,0.050f,0.060f,0.96f);
    c[ImGuiCol_ChildBg]       = ImVec4(0.065f,0.070f,0.080f,0.96f);
    c[ImGuiCol_Border]        = ImVec4(0.78f,0.80f,0.84f,0.55f);
    c[ImGuiCol_TitleBg]       = ImVec4(0.16f,0.015f,0.025f,1.00f);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.48f,0.02f,0.05f,1.00f);
    c[ImGuiCol_Button]        = ImVec4(0.34f,0.035f,0.065f,1.00f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.60f,0.045f,0.09f,1.00f);
    c[ImGuiCol_ButtonActive]  = ImVec4(0.82f,0.055f,0.11f,1.00f);
    c[ImGuiCol_Header]        = ImVec4(0.36f,0.035f,0.07f,0.95f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.62f,0.045f,0.10f,1.00f);
    c[ImGuiCol_HeaderActive]  = ImVec4(0.80f,0.055f,0.12f,1.00f);
    c[ImGuiCol_CheckMark]     = ImVec4(0.98f,0.13f,0.17f,1.00f);
}

static void setupImGui() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2((float)g_w,(float)g_h);
    ImGui_ImplOpenGL3_Init("#version 300 es");
    setupStyle();
    g_imguiReady = true;
    clock_gettime(CLOCK_MONOTONIC, &g_lastTs);
    flogf("[AZR06] ImGui ready display=%dx%d", g_w, g_h);
}

static bool agentReady() {
    return access("/data/local/tmp/AZTool/agent.ready", F_OK) == 0;
}

static void drawUI() {
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)g_w,(float)g_h);

    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    double dt = (double)(now.tv_sec - g_lastTs.tv_sec)
              + (double)(now.tv_nsec - g_lastTs.tv_nsec) / 1000000000.0;
    if (dt <= 0.0 || dt > 0.25) dt = 1.0 / 60.0;
    io.DeltaTime = (float)dt;
    g_lastTs = now;

    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();

    ImGui::SetNextWindowPos(ImVec2(28.0f,110.0f), ImGuiCond_Once);
    ImGui::SetNextWindowSize(ImVec2(84.0f,84.0f), ImGuiCond_Always);
    ImGuiWindowFlags iconFlags = ImGuiWindowFlags_NoTitleBar
        | ImGuiWindowFlags_NoResize
        | ImGuiWindowFlags_NoScrollbar
        | ImGuiWindowFlags_NoCollapse
        | ImGuiWindowFlags_NoSavedSettings;

    if (ImGui::Begin("##AZFloat", nullptr, iconFlags)) {
        ImGui::SetCursorPos(ImVec2(7,7));
        if (ImGui::Button("AZ", ImVec2(68,68))) {
            g_panelOpen = !g_panelOpen;
        }
    }
    ImGui::End();

    if (g_panelOpen) {
        ImGui::SetNextWindowPos(ImVec2(125.0f,90.0f), ImGuiCond_Once);
        ImGui::SetNextWindowSize(ImVec2((float)g_w*0.56f,(float)g_h*0.66f), ImGuiCond_Once);
        if (ImGui::Begin("AZ Tool OpenCore v0.6", &g_panelOpen)) {
            bool ready = agentReady();
            ImGui::Text("Renderer: x86_64 native EGL");
            ImGui::Text("ARM64 Agent: %s", ready ? "ONLINE" : "WAITING");
            ImGui::Separator();

            if (ImGui::BeginTabBar("AZTabs")) {
                if (ImGui::BeginTabItem("Core")) {
                    ImGui::TextWrapped("Dual-core path: UI on x86_64 host, IL2CPP on ARM64 translated guest.");
                    ImGui::Text("Screen: %d x %d", g_w, g_h);
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("IL2CPP")) {
                    ImGui::Text("Agent bridge: %s", ready ? "connected" : "not ready");
                    ImGui::TextDisabled("Assembly / Class / Method bridge follows this transport.");
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Script")) {
                    ImGui::TextDisabled("Lua runtime attaches to the ARM64 agent.");
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Settings")) {
                    ImGui::Text("AZ OpenCore renderer diagnostic");
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
        }
        ImGui::End();
    }

    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

static EGLBoolean hookSwap(EGLDisplay dpy, EGLSurface surf) {
    static unsigned long hits = 0;
    ++hits;

    EGLint w = 0;
    EGLint h = 0;
    eglQuerySurface(dpy,surf,EGL_WIDTH,&w);
    eglQuerySurface(dpy,surf,EGL_HEIGHT,&h);
    if (w > 0 && h > 0) {
        g_w = (int)w;
        g_h = (int)h;
    }

    if (hits <= 5) {
        flogf("[AZR06] swap HIT #%lu size=%dx%d", hits, g_w, g_h);
    }

    if (!g_imguiReady && g_w > 0 && g_h > 0) {
        setupImGui();
    }

    if (g_imguiReady) {
        drawUI();
    }

    return g_origSwap ? g_origSwap(dpy,surf) : EGL_FALSE;
}

static void* worker(void*) {
    flog("[AZR06] renderer worker started");

    void* egl = nullptr;
    for (int i=0; i<80 && !egl; ++i) {
        egl = dlopen("libEGL.so", RTLD_NOW | RTLD_LOCAL);
        if (!egl) usleep(250000);
    }

    if (!egl) {
        flog("[AZR06] libEGL dlopen failed");
        return nullptr;
    }

    void* p = dlsym(egl, "eglSwapBuffers");
    flogf("[AZR06] eglSwapBuffers=%p", p);
    if (!p) return nullptr;

    int rc = DobbyHook(p,(void*)hookSwap,(void**)&g_origSwap);
    flogf("[AZR06] DobbyHook EGL rc=%d orig=%p", rc, (void*)g_origSwap);
    if (rc == 0 && g_origSwap) {
        hookInput();
    }
    return nullptr;
}

extern "C" __attribute__((visibility("default")))
int az_renderer_alive() {
    return g_imguiReady ? 2 : 1;
}

__attribute__((constructor))
static void az_renderer_init() {
    pthread_t t{};
    if (pthread_create(&t,nullptr,worker,nullptr) == 0) {
        pthread_detach(t);
        flog("[AZR06] renderer constructor scheduled");
    } else {
        flog("[AZR06] renderer pthread_create failed");
    }
}
