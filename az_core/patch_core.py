from pathlib import Path
import shutil
import sys

if len(sys.argv) != 4:
    raise SystemExit("usage: patch_core.py <Il2CppTool> <AZ repo root> <Lua source root>")

root = Path(sys.argv[1])
az_root = Path(sys.argv[2])
lua_root = Path(sys.argv[3])
jni = root / "app/src/main/jni"

# Copy AZ-owned sources.
shutil.copy2(az_root / "az_core/AZEntry.cpp", jni / "AZEntry.cpp")
shutil.copy2(az_root / "az_core/AZLua.cpp", jni / "Tool/AZLua.cpp")
shutil.copy2(az_root / "az_core/AZLua.h", jni / "Tool/AZLua.h")

# Copy Lua 5.4.7 sources into the native project.
lua_dst = jni / "Lua"
if lua_dst.exists():
    shutil.rmtree(lua_dst)
lua_dst.mkdir(parents=True)
skip = {"lua.c", "luac.c", "onelua.c"}
for p in lua_root.glob("*"):
    if p.suffix in {".c", ".h"} and p.name not in skip:
        shutil.copy2(p, lua_dst / p.name)

required = ["lua.h", "lauxlib.h", "lualib.h", "lapi.c", "lvm.c"]
for name in required:
    if not (lua_dst / name).exists():
        raise SystemExit(f"missing Lua source: {name}")

# Build as libTool.so, matching the mature payload ABI shape.
mk = jni / "Android.mk"
s = mk.read_text(encoding="utf-8")
s = s.replace("LOCAL_MODULE := Il2CppTool", "LOCAL_MODULE := Tool", 1)
s = s.replace(
    "LOCAL_C_INCLUDES += $(LOCAL_PATH)\n\nLOCAL_SRC_FILES := Main.cpp \\\n",
    "LOCAL_C_INCLUDES += $(LOCAL_PATH)\n"
    "LOCAL_C_INCLUDES += $(LOCAL_PATH)/Lua\n\n"
    "LOCAL_SRC_FILES := Main.cpp \\\n"
    "    AZEntry.cpp \\\n"
    "    Tool/AZLua.cpp \\\n",
    1,
)
marker = "include $(BUILD_SHARED_LIBRARY)"
lua_block = """
# AZ-owned Lua 5.4.7 runtime.
LUA_SRC_FILES := $(wildcard $(LOCAL_PATH)/Lua/*.c)
LOCAL_SRC_FILES += $(LUA_SRC_FILES:$(LOCAL_PATH)/%=%)
LOCAL_CFLAGS += -DLUA_USE_POSIX
LOCAL_CPPFLAGS += -DLUA_USE_POSIX

"""
if marker not in s:
    raise SystemExit("Android.mk build marker missing")
s = s.replace(marker, lua_block + marker, 1)
mk.write_text(s, encoding="utf-8")

# Brand + add the scripting tab.
main = jni / "Main.cpp"
m = main.read_text(encoding="utf-8")
if '#include "Tool/AZLua.h"' not in m:
    m = m.replace('#include "Tool/SelfCheck.h"', '#include "Tool/SelfCheck.h"\n#include "Tool/AZLua.h"', 1)
m = m.replace(
    'const char *title = OBFUSCATE("Il2CppTool v0.9 | HitMargin");',
    'const char *title = OBFUSCATE("AZ Tool Core 0.1");',
    1,
)

trace_anchor = """
        // empty() 也是无锁读：追踪工作线程会持 hookerMtx 往里 insert/erase，
"""
script_tab = """
        if (ImGui::BeginTabItem("脚本"))
        {
            AZLua::Draw();
            ImGui::EndTabItem();
        }

"""
if trace_anchor not in m:
    raise SystemExit("Main.cpp tab anchor missing")
m = m.replace(trace_anchor, script_tab + trace_anchor, 1)

cleanup_anchor = """    Keyboard::Reset();
}"""
if cleanup_anchor not in m:
    raise SystemExit("Main.cpp cleanup anchor missing")
m = m.replace(cleanup_anchor, "    Keyboard::Reset();\n    AZLua::Shutdown();\n}", 1)
main.write_text(m, encoding="utf-8")

# AZ visual theme. Keep the existing rendering/input code; only theme it.
menu = jni / "Menu/ImGui.cpp"
u = menu.read_text(encoding="utf-8")
style_anchor = """        ImGuiStyle &style = ImGui::GetStyle();
        style.ScrollbarSize *= 2.5f;
"""
if style_anchor in u:
    theme = """        ImGuiStyle &style = ImGui::GetStyle();
        style.ScrollbarSize *= 2.5f;
        style.WindowRounding = 9.0f;
        style.ChildRounding = 7.0f;
        style.FrameRounding = 6.0f;
        style.PopupRounding = 7.0f;
        style.GrabRounding = 6.0f;
        ImVec4 *azc = style.Colors;
        azc[ImGuiCol_WindowBg]      = ImVec4(0.045f, 0.050f, 0.060f, 0.96f);
        azc[ImGuiCol_ChildBg]       = ImVec4(0.060f, 0.065f, 0.075f, 0.96f);
        azc[ImGuiCol_Border]        = ImVec4(0.76f, 0.78f, 0.82f, 0.55f);
        azc[ImGuiCol_TitleBg]       = ImVec4(0.15f, 0.02f, 0.03f, 1.00f);
        azc[ImGuiCol_TitleBgActive] = ImVec4(0.50f, 0.02f, 0.05f, 1.00f);
        azc[ImGuiCol_Button]        = ImVec4(0.34f, 0.04f, 0.07f, 1.00f);
        azc[ImGuiCol_ButtonHovered] = ImVec4(0.58f, 0.05f, 0.09f, 1.00f);
        azc[ImGuiCol_ButtonActive]  = ImVec4(0.78f, 0.06f, 0.11f, 1.00f);
        azc[ImGuiCol_Header]        = ImVec4(0.36f, 0.04f, 0.07f, 0.95f);
        azc[ImGuiCol_HeaderHovered] = ImVec4(0.62f, 0.05f, 0.10f, 1.00f);
        azc[ImGuiCol_HeaderActive]  = ImVec4(0.80f, 0.06f, 0.12f, 1.00f);
        azc[ImGuiCol_CheckMark]     = ImVec4(0.98f, 0.14f, 0.18f, 1.00f);
"""
    u = u.replace(style_anchor, theme, 1)
menu.write_text(u, encoding="utf-8")

print("AZ Core source patch complete")
