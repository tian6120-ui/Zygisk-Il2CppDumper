#pragma once

namespace AZLua {
bool Init();
void Shutdown();
void Draw();
void DrawRetainedWindows();
bool Run(const char* code);
const char* Version();
}
