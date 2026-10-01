from pathlib import Path
import sys

root = Path(sys.argv[1] if len(sys.argv) > 1 else "Il2CppTool")
main = root / "app/src/main/jni/Main.cpp"
menu = root / "app/src/main/jni/Menu/ImGui.cpp"

s = main.read_text(encoding="utf-8")
old = 'const char *title = OBFUSCATE("Il2CppTool v0.9 | HitMargin");'
new = 'const char *title = OBFUSCATE("AZ Tool OpenCore v0.1");'
if old not in s:
    raise SystemExit("Main.cpp title anchor not found")
s = s.replace(old, new, 1)

s = s.replace('ImGui::Button("bilibili HitMargin", ImVec2(-1, 0))',
              'ImGui::Button("AZ Tool | GPLv3 OpenCore", ImVec2(-1, 0))', 1)
s = s.replace('OBFUSCATE("https://m.bilibili.com/space/1757946676")',
              'OBFUSCATE("https://github.com/2228293026/Il2CppTool")', 1)

main.write_text(s, encoding="utf-8")

m = menu.read_text(encoding="utf-8")
anchor = '''        ImGuiStyle &style = ImGui::GetStyle();
        style.ScrollbarSize *= 2.5f;
'''
if anchor not in m:
    raise SystemExit("ImGui.cpp style anchor not found")
theme = '''        ImGuiStyle &style = ImGui::GetStyle();
        style.ScrollbarSize *= 2.5f;

        // AZ OpenCore theme: platinum / white / red, high contrast.
        style.WindowRounding = 9.0f;
        style.ChildRounding = 7.0f;
        style.FrameRounding = 6.0f;
        style.PopupRounding = 7.0f;
        style.ScrollbarRounding = 7.0f;
        style.GrabRounding = 6.0f;
        ImVec4 *c = style.Colors;
        c[ImGuiCol_WindowBg]         = ImVec4(0.055f, 0.060f, 0.070f, 0.96f);
        c[ImGuiCol_ChildBg]          = ImVec4(0.070f, 0.075f, 0.085f, 0.96f);
        c[ImGuiCol_PopupBg]          = ImVec4(0.060f, 0.065f, 0.075f, 0.98f);
        c[ImGuiCol_Border]           = ImVec4(0.72f, 0.74f, 0.78f, 0.55f);
        c[ImGuiCol_FrameBg]          = ImVec4(0.12f, 0.13f, 0.15f, 1.00f);
        c[ImGuiCol_FrameBgHovered]   = ImVec4(0.32f, 0.05f, 0.07f, 1.00f);
        c[ImGuiCol_FrameBgActive]    = ImVec4(0.52f, 0.04f, 0.07f, 1.00f);
        c[ImGuiCol_TitleBg]          = ImVec4(0.14f, 0.02f, 0.03f, 1.00f);
        c[ImGuiCol_TitleBgActive]    = ImVec4(0.50f, 0.02f, 0.05f, 1.00f);
        c[ImGuiCol_CheckMark]        = ImVec4(0.95f, 0.12f, 0.16f, 1.00f);
        c[ImGuiCol_SliderGrab]       = ImVec4(0.78f, 0.08f, 0.12f, 1.00f);
        c[ImGuiCol_SliderGrabActive] = ImVec4(1.00f, 0.15f, 0.20f, 1.00f);
        c[ImGuiCol_Button]           = ImVec4(0.34f, 0.04f, 0.07f, 1.00f);
        c[ImGuiCol_ButtonHovered]    = ImVec4(0.58f, 0.05f, 0.09f, 1.00f);
        c[ImGuiCol_ButtonActive]     = ImVec4(0.78f, 0.06f, 0.11f, 1.00f);
        c[ImGuiCol_Header]           = ImVec4(0.36f, 0.04f, 0.07f, 0.95f);
        c[ImGuiCol_HeaderHovered]    = ImVec4(0.62f, 0.05f, 0.10f, 1.00f);
        c[ImGuiCol_HeaderActive]     = ImVec4(0.80f, 0.06f, 0.12f, 1.00f);
        c[ImGuiCol_Separator]        = ImVec4(0.70f, 0.72f, 0.76f, 0.45f);
        c[ImGuiCol_ResizeGrip]       = ImVec4(0.72f, 0.08f, 0.12f, 0.45f);
        c[ImGuiCol_ResizeGripHovered]= ImVec4(0.92f, 0.10f, 0.15f, 0.75f);
        c[ImGuiCol_ResizeGripActive] = ImVec4(1.00f, 0.14f, 0.18f, 1.00f);
        c[ImGuiCol_Tab]              = ImVec4(0.18f, 0.03f, 0.05f, 1.00f);
        c[ImGuiCol_TabHovered]       = ImVec4(0.58f, 0.05f, 0.10f, 1.00f);
        c[ImGuiCol_TabActive]        = ImVec4(0.48f, 0.04f, 0.08f, 1.00f);
'''
m = m.replace(anchor, theme, 1)
menu.write_text(m, encoding="utf-8")

print("AZ OpenCore patch applied")
