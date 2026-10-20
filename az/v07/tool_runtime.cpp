#include "tool_runtime.h"
#include <android/log.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <vector>
#include "imgui.h"
#include "Il2cpp/Il2cpp.h"
#include "Tool/Tool.h"
#include "Tool/Keyboard.h"

#define AZTOOL_TAG "AZHF7TOOL"
#define AZTOOL_I(...) __android_log_print(ANDROID_LOG_INFO, AZTOOL_TAG, __VA_ARGS__)
#define AZTOOL_E(...) __android_log_print(ANDROID_LOG_ERROR, AZTOOL_TAG, __VA_ARGS__)

// Host state previously supplied by the old Main.cpp renderer.
// In S5 the HF7 payload itself is the only host/renderer.
Il2CppImage *g_Image = nullptr;
ImVec2 initialScreenSize{};

namespace {
std::atomic<int> g_state{0}; // 0=pending, 1=ready, -1=failed

Il2CppImage *chooseMainImage(const std::vector<Il2CppImage *> &images) {
    for (auto *image : images) {
        if (!image) continue;
        const char *name = image->getName();
        if (name && std::strstr(name, "Assembly-CSharp")) return image;
    }
    return images.empty() ? nullptr : images.front();
}
}

bool az_tool_ready() { return g_state.load(std::memory_order_acquire) == 1; }
bool az_tool_failed() { return g_state.load(std::memory_order_acquire) < 0; }

void az_tool_tick() {
    if (g_state.load(std::memory_order_acquire) != 0) return;
    if (!ImGui::GetCurrentContext()) return;

    if (!Il2cpp::Init() || !Il2cpp::EnsureAttached()) return;

    auto images = Il2cpp::GetImagesFresh();
    images.erase(std::remove(images.begin(), images.end(), nullptr), images.end());
    if (images.empty()) return;

    g_Image = chooseMainImage(images);
    if (!g_Image) return;

    try {
        initialScreenSize = ImGui::GetIO().DisplaySize;
        Keyboard::Init();
        Tool::Init(g_Image, images);
        g_state.store(1, std::memory_order_release);
        AZTOOL_I("TOOL_READY source=HF7 single-core images=%zu image=%s",
                 images.size(), g_Image->getName() ? g_Image->getName() : "?");
    } catch (...) {
        g_state.store(-1, std::memory_order_release);
        AZTOOL_E("TOOL_INIT_FAILED");
    }
}

void az_tool_draw() {
    az_tool_tick();
    if (!az_tool_ready()) return;
    Tool::Draw();
    Keyboard::Update();
}
