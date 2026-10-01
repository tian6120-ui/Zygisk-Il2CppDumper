from pathlib import Path
import sys

root = Path(sys.argv[1] if len(sys.argv) > 1 else "Il2CppTool")
main = root / "app/src/main/jni/Main.cpp"
menu = root / "app/src/main/jni/Menu/ImGui.cpp"

s = main.read_text(encoding="utf-8")
s = s.replace('const char *title = OBFUSCATE("Il2CppTool v0.9 | HitMargin");',
              'const char *title = OBFUSCATE("AZ Tool OpenCore v0.4");', 1)
s = s.replace('ImGui::Button("bilibili HitMargin", ImVec2(-1, 0))',
              'ImGui::Button("AZ Tool | GPLv3 OpenCore", ImVec2(-1, 0))', 1)
s = s.replace('OBFUSCATE("https://m.bilibili.com/space/1757946676")',
              'OBFUSCATE("https://github.com/2228293026/Il2CppTool")', 1)
main.write_text(s, encoding="utf-8")

m = menu.read_text(encoding="utf-8")
if '#include <dlfcn.h>' not in m:
    m = m.replace('#include <unistd.h>', '#include <unistd.h>\n#include <dlfcn.h>', 1)
m = m.replace(
'''void *initModMenu(void *menu_addr, void *on_init_addr, bool isJni)
{
    menuAddress = (void (*)())menu_addr;
    onInitAddr = (void (*)())on_init_addr;
''',
'''void *initModMenu(void *menu_addr, void *on_init_addr, bool isJni)
{
    LOGI("[AZOC04] initModMenu enter isJni=%d", isJni ? 1 : 0);
    menuAddress = (void (*)())menu_addr;
    onInitAddr = (void (*)())on_init_addr;
''', 1)

start = m.find('    // 字体字形范围必须在字体图集烘焙之前读到')
end = m.find('    // do\n', start)
if start != -1 and end != -1:
    m = m[:start] + '''    // MuMu/Houdini translated guest path: do not touch IL2CPP before the
    // runtime API table is initialized. Upstream ReadFontConfigEarly() reaches
    // UnityEngine.Application through IL2CPP and can stall in this environment.
    g_fontFullRangeRequested = false;
    LOGI("[AZOC04] early font config skipped; default glyph range");
''' + m[end:]

m = m.replace(
'''        while (!isLibraryLoaded(OBFUSCATE("libEGL.so")))
        {
            sleep(1);
        }
        auto swapBuffers = ((uintptr_t)DobbySymbolResolver(OBFUSCATE("libEGL.so"), OBFUSCATE("eglSwapBuffers")));
''',
'''        LOGI("[AZOC04] waiting for libEGL.so in process maps");
        int eglWait = 0;
        while (!isLibraryLoaded(OBFUSCATE("libEGL.so")))
        {
            sleep(1);
            ++eglWait;
            if (eglWait <= 5 || (eglWait % 5) == 0)
                LOGI("[AZOC04] libEGL wait=%d sec", eglWait);
            if (eglWait >= 20)
            {
                LOGE("[AZOC04] libEGL not visible after 20s");
                g_initState = INIT_FAILED;
                return nullptr;
            }
        }
        LOGI("[AZOC04] libEGL visible");
        void *eglHandle = dlopen("libEGL.so", RTLD_NOW | RTLD_LOCAL);
        LOGI("[AZOC04] dlopen(libEGL.so)=%p err=%s", eglHandle, eglHandle ? "none" : dlerror());
        void *eglDirect = eglHandle ? dlsym(eglHandle, "eglSwapBuffers") : nullptr;
        LOGI("[AZOC04] dlsym eglSwapBuffers=%p", eglDirect);
        auto swapBuffers = ((uintptr_t)DobbySymbolResolver(OBFUSCATE("libEGL.so"), OBFUSCATE("eglSwapBuffers")));
        LOGI("[AZOC04] DobbySymbolResolver eglSwapBuffers=%p", (void*)swapBuffers);
''', 1)

m = m.replace(
'''        KittyMemory::ProtectAddr((void *)swapBuffers, sizeof(swapBuffers), PROT_READ | PROT_WRITE | PROT_EXEC);
        if (DobbyHook((void *)swapBuffers, (void *)swapbuffers_hook, (void **)&o_swapbuffers) != 0 || !o_swapbuffers)
''',
'''        LOGI("[AZOC04] attempting EGL hook target=%p replacement=%p", (void*)swapBuffers, (void*)swapbuffers_hook);
        bool protectOk = KittyMemory::ProtectAddr((void *)swapBuffers, sizeof(swapBuffers), PROT_READ | PROT_WRITE | PROT_EXEC);
        LOGI("[AZOC04] ProtectAddr=%d", protectOk ? 1 : 0);
        int hookRc = DobbyHook((void *)swapBuffers, (void *)swapbuffers_hook, (void **)&o_swapbuffers);
        LOGI("[AZOC04] DobbyHook rc=%d orig=%p", hookRc, (void*)o_swapbuffers);
        if (hookRc != 0 || !o_swapbuffers)
''', 1)

anchor = '''        ImGuiStyle &style = ImGui::GetStyle();
        style.ScrollbarSize *= 2.5f;
'''
if anchor in m:
    theme = '''        ImGuiStyle &style = ImGui::GetStyle();
        style.ScrollbarSize *= 2.5f;
        style.WindowRounding = 9.0f;
        style.ChildRounding = 7.0f;
        style.FrameRounding = 6.0f;
        style.PopupRounding = 7.0f;
        ImVec4 *c = style.Colors;
        c[ImGuiCol_WindowBg]      = ImVec4(0.055f,0.060f,0.070f,0.96f);
        c[ImGuiCol_TitleBg]       = ImVec4(0.14f,0.02f,0.03f,1.00f);
        c[ImGuiCol_TitleBgActive] = ImVec4(0.50f,0.02f,0.05f,1.00f);
        c[ImGuiCol_Button]        = ImVec4(0.34f,0.04f,0.07f,1.00f);
        c[ImGuiCol_ButtonHovered] = ImVec4(0.58f,0.05f,0.09f,1.00f);
        c[ImGuiCol_ButtonActive]  = ImVec4(0.78f,0.06f,0.11f,1.00f);
        c[ImGuiCol_Header]        = ImVec4(0.36f,0.04f,0.07f,0.95f);
        c[ImGuiCol_HeaderHovered] = ImVec4(0.62f,0.05f,0.10f,1.00f);
        c[ImGuiCol_HeaderActive]  = ImVec4(0.80f,0.06f,0.12f,1.00f);
'''
    m = m.replace(anchor, theme, 1)

menu.write_text(m, encoding="utf-8")
print("AZ OpenCore v0.4 diagnostic patch applied")
